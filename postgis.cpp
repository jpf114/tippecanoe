#include "postgis.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <libpq-fe.h>
#include <string.h>
#include <unistd.h>
#include "geometry.hpp"
#include "serial.hpp"
#include "geojson.hpp"

PostGISReader::PostGISReader(const postgis_config &cfg) : config(cfg), conn(NULL)
{
    // Validate and adjust batch size
    if (config.batch_size < MIN_BATCH_SIZE)
    {
        config.batch_size = MIN_BATCH_SIZE;
    }
    else if (config.batch_size > MAX_BATCH_SIZE)
    {
        config.batch_size = MAX_BATCH_SIZE;
    }
}

PostGISReader::~PostGISReader()
{
    if (conn)
    {
        PQfinish((PGconn *)conn);
    }
}

bool PostGISReader::connect()
{
    // Use PQconnectdbParams for safer connection string construction
    const char *keywords[] = {"host", "port", "dbname", "user", "password", "connect_timeout", NULL};
    const char *values[] = {
        config.host.c_str(),
        config.port.c_str(),
        config.dbname.c_str(),
        config.user.c_str(),
        config.password.c_str(),
        std::to_string(CONNECTION_TIMEOUT_SEC).c_str(),
        NULL};
    conn = PQconnectdbParams(keywords, values, 0);

    if (PQstatus((PGconn *)conn) != CONNECTION_OK)
    {
        fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage((PGconn *)conn));
        return false;
    }

    return true;
}

bool PostGISReader::check_memory_usage()
{
    // Estimate current memory usage (simplified)
    size_t estimated = current_memory_usage.load();
    size_t limit = config.max_memory_mb * 1024 * 1024;
    
    if (estimated > limit)
    {
        fprintf(stderr, "Warning: Memory usage (%zu MB) exceeds limit (%zu MB)\n",
                estimated / (1024 * 1024), config.max_memory_mb);
        return false;
    }
    return true;
}

void PostGISReader::log_progress(size_t processed, size_t total, const char *stage)
{
    if (!config.enable_progress_report)
        return;

    if (total > 0)
    {
        double percent = (processed * 100.0) / total;
        fprintf(stderr, "Progress: %s - %zu/%zu (%.1f%%)\n", stage, processed, total, percent);
    }
    else
    {
        fprintf(stderr, "Progress: %s - %zu features processed\n", stage, processed);
    }
}

std::string PostGISReader::escape_json_string(const char *value)
{
    if (!value)
        return "";

    std::string result;
    const size_t len = strlen(value);
    result.reserve(len * 2);

    for (size_t k = 0; k < len; k++)
    {
        char c = value[k];
        switch (c)
        {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            // Handle control characters
            if (c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                result += buf;
            }
            else
            {
                result += c;
            }
            break;
        }
    }

    return result;
}

void PostGISReader::process_feature(PGresult *res, int row, int nfields, int geom_field_index,
                                    std::vector<struct serialization_state> &sst, size_t layer,
                                    const std::string &layername)
{
    // Reuse feature buffer to reduce allocations
    feature_buffer.clear();
    feature_buffer.reserve(1024); // Initial reservation

    // Build GeoJSON feature
    feature_buffer += "{\"type\":\"Feature\",";

    // Add geometry
    char *geom_value = PQgetvalue(res, row, geom_field_index);
    if (geom_value == NULL || strlen(geom_value) == 0)
    {
        return; // Skip features without geometry
    }

    feature_buffer += "\"geometry\":";
    feature_buffer += geom_value;

    // Check for properties
    bool has_properties = false;
    for (int field = 0; field < nfields; field++)
    {
        if (field != geom_field_index)
        {
            char *value = PQgetvalue(res, row, field);
            if (value != NULL && strlen(value) > 0)
            {
                has_properties = true;
                break;
            }
        }
    }

    // Add properties if present
    if (has_properties)
    {
        feature_buffer += ",\"properties\":{";
        bool first_property = true;

        for (int field = 0; field < nfields; field++)
        {
            if (field != geom_field_index)
            {
                const char *fieldname = PQfname(res, field);
                if (fieldname == NULL || strlen(fieldname) == 0 ||
                    strcmp(fieldname, config.geometry_field.c_str()) == 0)
                {
                    continue;
                }

                char *value = PQgetvalue(res, row, field);
                if (value == NULL || strlen(value) == 0)
                {
                    continue;
                }

                if (!first_property)
                {
                    feature_buffer += ",";
                }
                first_property = false;

                feature_buffer += "\"";
                feature_buffer += fieldname;
                feature_buffer += "\":";

                Oid type = PQftype(res, field);

                if (type == 20 || type == 21 || type == 23 || type == 700 || type == 701)
                {
                    // Numeric type - no escaping needed
                    feature_buffer += value;
                }
                else if (type == 16)
                {
                    // Boolean type
                    feature_buffer += (strcmp(value, "t") == 0) ? "true" : "false";
                }
                else
                {
                    // String type - escape special characters
                    feature_buffer += "\"";
                    feature_buffer += escape_json_string(value);
                    feature_buffer += "\"";
                }
            }
        }
        feature_buffer += "}";
    }

    feature_buffer += "}";

    // Parse the feature immediately (streaming approach)
    char *feature_buffer_copy = strdup(feature_buffer.c_str());
    if (feature_buffer_copy)
    {
        struct json_pull *jp = json_begin_map(feature_buffer_copy, feature_buffer.length());
        if (jp != NULL)
        {
            struct serialization_state temp_sst = sst[layer % sst.size()];
            temp_sst.fname = "PostGIS";
            temp_sst.line = row + 1;

            parse_json(&temp_sst, jp, layer, layername);
            json_end_map(jp);
        }
        free(feature_buffer_copy);
    }

    // Update statistics
    total_features_processed.fetch_add(1);

    // Update memory tracking (rough estimate)
    current_memory_usage.fetch_add(feature_buffer.capacity());
}

void PostGISReader::process_batch(PGresult *res, std::vector<struct serialization_state> &sst,
                                  size_t layer, const std::string &layername, int geom_field_index)
{
    int ntuples = PQntuples(res);
    int nfields = PQnfields(res);

    for (int i = 0; i < ntuples; i++)
    {
        process_feature(res, i, nfields, geom_field_index, sst, layer, layername);

        // Periodically check memory usage
        if (i % 100 == 0 && !check_memory_usage())
        {
            fprintf(stderr, "Warning: Memory pressure detected at feature %d\n", i);
        }
    }

    // Update batch statistics
    total_batches_processed.fetch_add(1);

    // Log progress
    log_progress(total_features_processed.load(), 0, "Processing features");
}

bool PostGISReader::execute_query_with_retry(const std::string &query)
{
    int retries = 0;
    while (retries < config.max_retries)
    {
        if (execute_query(query))
        {
            return true;
        }

        retries++;
        if (retries < config.max_retries)
        {
            fprintf(stderr, "Retrying query (attempt %d/%d)...\n", retries + 1, config.max_retries);
            usleep(1000000); // Wait 1 second before retry
        }
    }

    return false;
}

bool PostGISReader::execute_query(const std::string &query)
{
    if (!conn)
    {
        fprintf(stderr, "Error: Not connected to database\n");
        return false;
    }

    PGresult *res = PQexec((PGconn *)conn, query.c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK)
    {
        fprintf(stderr, "Query failed: %s\n", PQerrorMessage((PGconn *)conn));
        PQclear(res);
        return false;
    }

    PQclear(res);
    return true;
}

bool PostGISReader::read_features(std::vector<struct serialization_state> &sst, size_t layer, const std::string &layername)
{
    if (!conn)
    {
        if (!connect())
        {
            return false;
        }
    }

    // Build base query
    std::string base_query;
    if (!config.sql.empty())
    {
        base_query = config.sql;
    }
    else if (!config.table.empty() && !config.geometry_field.empty())
    {
        base_query = "SELECT ST_AsGeoJSON(" + config.geometry_field + ") as geojson, * FROM " + config.table;
    }
    else
    {
        fprintf(stderr, "Error: Either --postgis-sql or both --postgis-table and --postgis-geometry-field are required\n");
        return false;
    }

    // Determine total count for progress reporting
    size_t total_count = 0;
    if (config.enable_progress_report)
    {
        std::string count_query = "SELECT COUNT(*) FROM (" + base_query + ") AS subquery";
        PGresult *count_res = PQexec((PGconn *)conn, count_query.c_str());
        if (PQresultStatus(count_res) == PGRES_TUPLES_OK)
        {
            total_count = strtoull(PQgetvalue(count_res, 0, 0), NULL, 10);
        }
        PQclear(count_res);
    }

    // Process data in batches using cursor if enabled
    if (config.use_cursor && total_count > config.batch_size)
    {
        fprintf(stderr, "Using cursor-based batch processing (batch size: %zu)\n", config.batch_size);

        // Declare cursor
        std::string cursor_name = "postgis_cursor_" + std::to_string(getpid());
        std::string declare_query = "DECLARE " + cursor_name + " CURSOR FOR " + base_query;

        if (!execute_query(declare_query))
        {
            fprintf(stderr, "Failed to declare cursor, falling back to non-cursor mode\n");
            // Fall back to non-cursor mode
            goto non_cursor_mode;
        }

        // Fetch and process batches
        size_t offset = 0;
        while (true)
        {
            std::string fetch_query = "FETCH FORWARD " + std::to_string(config.batch_size) + " FROM " + cursor_name;
            PGresult *res = PQexec((PGconn *)conn, fetch_query.c_str());

            if (PQresultStatus(res) != PGRES_TUPLES_OK)
            {
                fprintf(stderr, "Fetch failed: %s\n", PQerrorMessage((PGconn *)conn));
                PQclear(res);
                break;
            }

            int ntuples = PQntuples(res);
            if (ntuples == 0)
            {
                PQclear(res);
                break; // No more data
            }

            // Find geometry column
            int geom_field_index = -1;
            int nfields = PQnfields(res);
            for (int i = 0; i < nfields; i++)
            {
                if (strcmp(PQfname(res, i), "geojson") == 0)
                {
                    geom_field_index = i;
                    break;
                }
            }
            if (geom_field_index == -1)
            {
                geom_field_index = 0;
            }

            // Process this batch
            process_batch(res, sst, layer, layername, geom_field_index);

            offset += ntuples;
            log_progress(offset, total_count, "Fetching batches");

            PQclear(res);

            // Check memory and potentially pause
            if (!check_memory_usage())
            {
                fprintf(stderr, "Memory pressure detected, pausing...\n");
                usleep(100000); // Brief pause
            }
        }

        // Close cursor
        std::string close_query = "CLOSE " + cursor_name;
        execute_query(close_query);
    }
    else
    {
    non_cursor_mode:
        fprintf(stderr, "Using standard query mode\n");

        // Execute query
        PGresult *res = PQexec((PGconn *)conn, base_query.c_str());
        if (PQresultStatus(res) != PGRES_TUPLES_OK)
        {
            fprintf(stderr, "Query failed: %s\n", PQerrorMessage((PGconn *)conn));
            PQclear(res);
            return false;
        }

        int ntuples = PQntuples(res);
        fprintf(stderr, "Processing %d features\n", ntuples);

        // Find geometry column
        int geom_field_index = -1;
        int nfields = PQnfields(res);
        for (int i = 0; i < nfields; i++)
        {
            if (strcmp(PQfname(res, i), "geojson") == 0)
            {
                geom_field_index = i;
                break;
            }
        }

        if (geom_field_index == -1 && !config.geometry_field.empty())
        {
            for (int i = 0; i < nfields; i++)
            {
                if (strcmp(PQfname(res, i), config.geometry_field.c_str()) == 0)
                {
                    geom_field_index = i;
                    break;
                }
            }
        }

        if (geom_field_index == -1)
        {
            fprintf(stderr, "Warning: Geometry column not found, using first column\n");
            geom_field_index = 0;
        }

        // Process all features
        process_batch(res, sst, layer, layername, geom_field_index);

        PQclear(res);
    }

    // Final progress report
    log_progress(total_features_processed.load(), total_count, "Completed");
    fprintf(stderr, "Total features processed: %zu in %zu batches\n",
            total_features_processed.load(), total_batches_processed.load());

    return true;
}
