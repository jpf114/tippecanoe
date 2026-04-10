#include "postgis.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <libpq-fe.h>
#include <string.h>
#include <unistd.h>
#include "geometry.hpp"
#include "serial.hpp"
#include "geojson.hpp"
#include "wkb_parser.hpp"
#include "error_logger.hpp"

int PostGISReader::cached_srid_ = -1;
bool PostGISReader::srid_cached_ = false;

PostGISReader::PostGISReader(const postgis_config &cfg) : config(cfg), conn(NULL)
{
    if (config.batch_size < MIN_POSTGIS_BATCH_SIZE)
    {
        config.batch_size = MIN_POSTGIS_BATCH_SIZE;
    }
    else if (config.batch_size > MAX_POSTGIS_BATCH_SIZE)
    {
        config.batch_size = MAX_POSTGIS_BATCH_SIZE;
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
    const char *keywords[] = {"host", "port", "dbname", "user", "password", "connect_timeout", NULL};
    std::string timeout_str = std::to_string(POSTGIS_CONNECTION_TIMEOUT_SEC);
    const char *values[] = {
        config.host.c_str(),
        config.port.c_str(),
        config.dbname.c_str(),
        config.user.c_str(),
        config.password.c_str(),
        timeout_str.c_str(),
        NULL};

    DEBUG_LOG("Connecting to PostGIS (%s:%s/%s)...", config.host.c_str(), config.port.c_str(), config.dbname.c_str());
    conn = PQconnectdbParams(keywords, values, 0);

    if (PQstatus((PGconn *)conn) != CONNECTION_OK)
    {
        fprintf(stderr, "Error: Connection to PostGIS failed: %s\n", PQerrorMessage((PGconn *)conn));
        return false;
    }

    DEBUG_LOG("Connected to PostGIS successfully.");
    return true;
}

void PostGISReader::disconnect()
{
    if (conn)
    {
        PQfinish((PGconn *)conn);
        conn = NULL;
    }
}

int PostGISReader::get_cached_srid(const postgis_config &cfg, void *conn_ptr)
{
    if (srid_cached_)
    {
        return cached_srid_;
    }

    if (!conn_ptr)
    {
        return 0;
    }

    PGconn *pgconn = static_cast<PGconn *>(conn_ptr);
    std::string srid_query;

    if (!cfg.sql.empty())
    {
        srid_query = "SELECT ST_SRID((" + cfg.sql + ")::geometry) LIMIT 1";
    }
    else if (!cfg.table.empty() && !cfg.geometry_field.empty())
    {
        char *esc_geom = PQescapeIdentifier(pgconn, cfg.geometry_field.c_str(), cfg.geometry_field.size());
        char *esc_table = PQescapeIdentifier(pgconn, cfg.table.c_str(), cfg.table.size());

        if (!esc_geom || !esc_table)
        {
            fprintf(stderr, "Error: Failed to escape SQL identifiers: %s\n", PQerrorMessage(pgconn));
            PQfreemem(esc_geom);
            PQfreemem(esc_table);
            cached_srid_ = 0;
            srid_cached_ = true;
            return 0;
        }

        srid_query = "SELECT ST_SRID(" + std::string(esc_geom) + ") FROM " + std::string(esc_table) + " LIMIT 1";
        PQfreemem(esc_geom);
        PQfreemem(esc_table);
    }
    else
    {
        cached_srid_ = 0;
        srid_cached_ = true;
        return 0;
    }

    PGresult *res = PQexec(pgconn, srid_query.c_str());
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0 && !PQgetisnull(res, 0, 0))
    {
        cached_srid_ = atoi(PQgetvalue(res, 0, 0));
    }
    else
    {
        cached_srid_ = 0;
    }
    PQclear(res);

    srid_cached_ = true;
    DEBUG_LOG("Cached geometry SRID = %d", cached_srid_);
    return cached_srid_;
}

std::string PostGISReader::build_select_query(const postgis_config &cfg, int srid)
{
    if (!cfg.sql.empty())
    {
        if (srid == 4326)
        {
            return "SELECT ST_AsBinary((" + cfg.sql + ")::geometry) AS wkb, * FROM (" + cfg.sql + ") AS _subq";
        }
        else
        {
            return "SELECT ST_AsBinary(ST_Transform((" + cfg.sql + ")::geometry, 4326)) AS wkb, * FROM (" + cfg.sql + ") AS _subq";
        }
    }

    std::string esc_geom_str = "\"" + cfg.geometry_field + "\"";
    std::string esc_table_str = "\"" + cfg.table + "\"";

    if (srid == 4326)
    {
        return "SELECT ST_AsBinary(" + esc_geom_str + ") AS wkb, * FROM " + esc_table_str;
    }
    else
    {
        return "SELECT ST_AsBinary(ST_Transform(" + esc_geom_str + ", 4326)) AS wkb, * FROM " + esc_table_str;
    }
}

bool PostGISReader::check_memory_usage()
{
    size_t estimated = current_memory_usage.load();
    size_t limit = config.max_memory_mb * 1024 * 1024;

    if (estimated > limit)
    {
        fprintf(stderr, "ERROR: Memory usage (%zu MB) exceeds limit (%zu MB)\n",
                estimated / (1024 * 1024), config.max_memory_mb);
        return false;
    }

    if (estimated > limit * 0.8)
    {
        fprintf(stderr, "WARNING: Memory usage at %.1f%% (%zu MB / %zu MB)\n",
                (double)estimated / limit * 100.0,
                estimated / (1024 * 1024),
                config.max_memory_mb);
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

std::vector<uint8_t> PostGISReader::decode_bytea(const char *hex_data, size_t hex_len)
{
    std::vector<uint8_t> result;

    if (hex_len >= 2 && hex_data[0] == '\\' && hex_data[1] == 'x')
    {
        hex_data += 2;
        hex_len -= 2;
    }

    if (hex_len % 2 != 0)
    {
        return result;
    }

    result.reserve(hex_len / 2);

    for (size_t i = 0; i < hex_len; i += 2)
    {
        uint8_t hi = 0, lo = 0;
        char ch = hex_data[i];
        if (ch >= '0' && ch <= '9') hi = ch - '0';
        else if (ch >= 'a' && ch <= 'f') hi = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') hi = ch - 'A' + 10;
        else return std::vector<uint8_t>();

        ch = hex_data[i + 1];
        if (ch >= '0' && ch <= '9') lo = ch - '0';
        else if (ch >= 'a' && ch <= 'f') lo = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') lo = ch - 'A' + 10;
        else return std::vector<uint8_t>();

        result.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }

    return result;
}

void PostGISReader::process_feature(PGresult *res, int row, int nfields, int wkb_field_index,
                                    const std::vector<std::string> &field_names,
                                    std::vector<struct serialization_state> &sst, size_t layer,
                                    const std::string &layername, size_t thread_id)
{
    if (PQgetisnull(res, row, wkb_field_index))
    {
        parse_errors_.fetch_add(1);
        ErrorLogger::instance().log_parse_error(row + 1, "NULL", "NULL WKB geometry", "");
        return;
    }

    char *wkb_hex = PQgetvalue(res, row, wkb_field_index);
    int wkb_len = PQgetlength(res, row, wkb_field_index);

    if (wkb_hex == NULL || wkb_len == 0)
    {
        parse_errors_.fetch_add(1);
        ErrorLogger::instance().log_parse_error(row + 1, "EMPTY", "Empty WKB data", "");
        return;
    }

    std::vector<uint8_t> wkb_bytes = decode_bytea(wkb_hex, wkb_len);
    if (wkb_bytes.empty())
    {
        parse_errors_.fetch_add(1);
        std::string preview(wkb_hex, std::min(wkb_len, 60));
        ErrorLogger::instance().log_parse_error(row + 1, "BYTEA", "Failed to decode bytea data", preview);
        return;
    }

    WKBResult result = parse_wkb(wkb_bytes.data(), wkb_bytes.size());
    if (!result.valid)
    {
        parse_errors_.fetch_add(1);
        std::string preview(wkb_hex, std::min(wkb_len, 60));
        ErrorLogger::instance().log_parse_error(row + 1, "WKB_PARSE", result.error, preview);
        return;
    }

    size_t coord_memory = result.coordinates.capacity() * sizeof(draw);
    current_memory_usage.fetch_add(coord_memory);

    if (result.geometry_type < 0 || result.geometry_type >= GEOM_TYPES)
    {
        parse_errors_.fetch_add(1);
        ErrorLogger::instance().log_parse_error(row + 1, "UNSUPPORTED",
            "Unsupported geometry type: " + std::to_string(result.geometry_type), "");
        current_memory_usage.fetch_sub(coord_memory);
        return;
    }

    serial_feature sf;
    sf.layer = layer;
    sf.segment = thread_id % sst.size();
    sf.t = mb_geometry[result.geometry_type];
    sf.geometry = result.coordinates;
    sf.has_id = false;
    sf.id = 0;
    sf.tippecanoe_minzoom = -1;
    sf.tippecanoe_maxzoom = -1;
    sf.feature_minzoom = 0;
    sf.seq = *(sst[thread_id % sst.size()].layer_seq);

    std::vector<std::shared_ptr<std::string>> full_keys;
    std::vector<serial_val> values;

    size_t attr_memory = 0;

    for (int field = 0; field < nfields; field++)
    {
        if (field == wkb_field_index)
            continue;

        const std::string &fieldname = field_names[field];
        if (fieldname.empty() || fieldname == config.geometry_field || fieldname == "wkb")
        {
            continue;
        }

        if (PQgetisnull(res, row, field))
            continue;

        char *value = PQgetvalue(res, row, field);
        if (value == NULL || value[0] == '\0')
            continue;

        full_keys.emplace_back(std::make_shared<std::string>(fieldname));
        attr_memory += fieldname.size() + sizeof(std::string);

        serial_val sv;
        Oid type = PQftype(res, field);
        if (type == 20 || type == 21 || type == 23 || type == 700 || type == 701)
        {
            sv.type = mvt_double;
            sv.s = value;
        }
        else if (type == 16)
        {
            sv.type = mvt_bool;
            sv.s = (value[0] == 't') ? "true" : "false";
        }
        else
        {
            sv.type = mvt_string;
            sv.s = value;
        }
        values.push_back(sv);
        attr_memory += strlen(value) + 32;
    }

    sf.full_keys = std::move(full_keys);
    sf.full_values = std::move(values);

    current_memory_usage.fetch_add(attr_memory);

    struct serialization_state temp_sst = sst[thread_id % sst.size()];
    temp_sst.fname = "PostGIS";
    temp_sst.line = row + 1;

    serialize_feature(&temp_sst, sf, layername);

    current_memory_usage.fetch_sub(coord_memory + attr_memory);

    total_features_processed.fetch_add(1);
}

void PostGISReader::process_batch(PGresult *res, std::vector<struct serialization_state> &sst,
                                  size_t layer, const std::string &layername, int wkb_field_index,
                                  size_t thread_id)
{
    int ntuples = PQntuples(res);
    int nfields = PQnfields(res);

    std::vector<std::string> field_names;
    field_names.reserve(nfields);
    for (int i = 0; i < nfields; i++)
    {
        field_names.push_back(PQfname(res, i));
    }

    for (int i = 0; i < ntuples; i++)
    {
        process_feature(res, i, nfields, wkb_field_index, field_names, sst, layer, layername, thread_id);

        if (i % 100 == 0 && !check_memory_usage())
        {
            fprintf(stderr, "Memory pressure at feature %d, pausing...\n", i);
            usleep(100000);
        }
    }

    total_batches_processed.fetch_add(1);
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
            usleep(1000000);
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

bool PostGISReader::read_features(std::vector<struct serialization_state> &sst, size_t layer, const std::string &layername,
                                  size_t thread_id, size_t num_threads)
{
    if (!conn)
    {
        if (!connect())
        {
            return false;
        }
    }

    int srid = get_cached_srid(config, conn);

    std::string base_query;
    if (!config.sql.empty())
    {
        base_query = build_select_query(config, srid);
    }
    else if (!config.table.empty() && !config.geometry_field.empty())
    {
        base_query = build_select_query(config, srid);
    }
    else
    {
        fprintf(stderr, "Error: Either --postgis-sql or both --postgis-table and --postgis-geometry-field are required\n");
        return false;
    }

    if (num_threads > 1)
    {
        std::string shard_cond = "(abs(hashtext(ctid::text)) % " + std::to_string(num_threads) + ") = " + std::to_string(thread_id);

        size_t from_pos = base_query.find(" FROM ");
        if (from_pos != std::string::npos)
        {
            size_t where_pos = base_query.find(" WHERE ", from_pos);
            if (where_pos != std::string::npos)
            {
                base_query.insert(where_pos + strlen(" WHERE "), shard_cond + " AND ");
            }
            else
            {
                size_t group_pos = base_query.find(" GROUP BY ", from_pos);
                size_t order_pos = base_query.find(" ORDER BY ", from_pos);
                size_t limit_pos = base_query.find(" LIMIT ", from_pos);
                size_t insert_pos = base_query.size();

                if (group_pos != std::string::npos) insert_pos = group_pos;
                else if (order_pos != std::string::npos) insert_pos = order_pos;
                else if (limit_pos != std::string::npos) insert_pos = limit_pos;

                base_query.insert(insert_pos, " WHERE " + shard_cond);
            }
        }

        std::string probe_query = "SELECT 1 FROM (" + base_query + ") AS _probe LIMIT 1";
        PGresult *probe_res = PQexec((PGconn *)conn, probe_query.c_str());
        if (PQresultStatus(probe_res) != PGRES_TUPLES_OK)
        {
            fprintf(stderr, "Thread %zu: ctid sharding not supported (partitioned table or subquery), falling back to sequential read\n", thread_id);
            PQclear(probe_res);

            base_query = build_select_query(config, srid);
            if (thread_id != 0)
            {
                log_progress(0, 0, "Non-primary thread exiting (no sharding support)");
                return true;
            }
        }
        else
        {
            DEBUG_LOG("Thread %zu/%zu: using hash-based sharding on ctid", thread_id, num_threads);
        }
        PQclear(probe_res);
    }

    size_t total_count = 0;
    if (config.enable_progress_report)
    {
        std::string count_query = "SELECT COUNT(*) FROM (" + base_query + ") AS _count_subq";
        DEBUG_LOG("Executing count query");
        PGresult *count_res = PQexec((PGconn *)conn, count_query.c_str());
        if (PQresultStatus(count_res) == PGRES_TUPLES_OK && PQntuples(count_res) > 0)
        {
            total_count = strtoull(PQgetvalue(count_res, 0, 0), NULL, 10);
        }
        else
        {
            DEBUG_LOG("Count query failed: %s", PQerrorMessage((PGconn *)conn));
        }
        PQclear(count_res);
    }

    if (config.use_cursor && total_count > config.batch_size)
    {
        fprintf(stderr, "Thread %zu: Using cursor-based batch processing (batch size: %zu)\n",
                thread_id, config.batch_size);

        std::string cursor_name = "pg_cursor_" + std::to_string(getpid()) + "_" + std::to_string(thread_id);
        std::string begin_query = "BEGIN ISOLATION LEVEL REPEATABLE READ";
        std::string declare_query = "DECLARE " + cursor_name + " SCROLL CURSOR FOR " + base_query;

        bool cursor_ok = false;
        if (execute_query(begin_query))
        {
            if (execute_query(declare_query))
            {
                cursor_ok = true;
            }
            else
            {
                fprintf(stderr, "Thread %zu: Failed to declare cursor, falling back to non-cursor mode\n", thread_id);
                execute_query("ROLLBACK");
            }
        }
        else
        {
            fprintf(stderr, "Thread %zu: Failed to begin transaction, falling back to non-cursor mode\n", thread_id);
        }

        if (cursor_ok)
        {
            size_t offset = 0;
            while (true)
            {
                std::string fetch_query = "FETCH FORWARD " + std::to_string(config.batch_size) + " FROM " + cursor_name;
                PGresult *res = PQexec((PGconn *)conn, fetch_query.c_str());

                if (PQresultStatus(res) != PGRES_TUPLES_OK)
                {
                    fprintf(stderr, "Thread %zu: Fetch failed: %s\n", thread_id, PQerrorMessage((PGconn *)conn));
                    PQclear(res);
                    break;
                }

                int ntuples = PQntuples(res);
                if (ntuples == 0)
                {
                    PQclear(res);
                    break;
                }

                int wkb_field_index = -1;
                int nfields = PQnfields(res);

                for (int i = 0; i < nfields; i++)
                {
                    if (strcmp(PQfname(res, i), "wkb") == 0)
                    {
                        wkb_field_index = i;
                        break;
                    }
                }

                if (wkb_field_index == -1)
                {
                    fprintf(stderr, "Thread %zu: WKB geometry field not found in cursor result\n", thread_id);
                    PQclear(res);
                    execute_query("CLOSE " + cursor_name);
                    execute_query("COMMIT");
                    return false;
                }

                process_batch(res, sst, layer, layername, wkb_field_index, thread_id);

                offset += ntuples;
                log_progress(offset, total_count, "Fetching batches");

                PQclear(res);

                if (!check_memory_usage())
                {
                    fprintf(stderr, "Thread %zu: Memory pressure detected, pausing batch fetch...\n", thread_id);
                    usleep(500000);
                }
            }

            execute_query("CLOSE " + cursor_name);
            execute_query("COMMIT");
        }
        else
        {
            fprintf(stderr, "Thread %zu: Using standard query mode\n", thread_id);

            PGresult *res = PQexec((PGconn *)conn, base_query.c_str());
            if (PQresultStatus(res) != PGRES_TUPLES_OK)
            {
                fprintf(stderr, "Thread %zu: Query failed: %s\n", thread_id, PQerrorMessage((PGconn *)conn));
                PQclear(res);
                return false;
            }

            int ntuples = PQntuples(res);
            fprintf(stderr, "Thread %zu: Processing %d features\n", thread_id, ntuples);

            int wkb_field_index = -1;
            int nfields = PQnfields(res);

            for (int i = 0; i < nfields; i++)
            {
                if (strcmp(PQfname(res, i), "wkb") == 0)
                {
                    wkb_field_index = i;
                    break;
                }
            }

            if (wkb_field_index == -1)
            {
                fprintf(stderr, "Thread %zu: WKB geometry field not found in query result\n", thread_id);
                fprintf(stderr, "Available fields: ");
                for (int i = 0; i < nfields; i++)
                {
                    fprintf(stderr, "%s ", PQfname(res, i));
                }
                fprintf(stderr, "\n");
                PQclear(res);
                return false;
            }

            process_batch(res, sst, layer, layername, wkb_field_index, thread_id);

            PQclear(res);
        }
    }
    else
    {
        fprintf(stderr, "Thread %zu: Using standard query mode\n", thread_id);

        PGresult *res = PQexec((PGconn *)conn, base_query.c_str());
        if (PQresultStatus(res) != PGRES_TUPLES_OK)
        {
            fprintf(stderr, "Thread %zu: Query failed: %s\n", thread_id, PQerrorMessage((PGconn *)conn));
            PQclear(res);
            return false;
        }

        int ntuples = PQntuples(res);
        fprintf(stderr, "Thread %zu: Processing %d features\n", thread_id, ntuples);

        int wkb_field_index = -1;
        int nfields = PQnfields(res);

        for (int i = 0; i < nfields; i++)
        {
            if (strcmp(PQfname(res, i), "wkb") == 0)
            {
                wkb_field_index = i;
                break;
            }
        }

        if (wkb_field_index == -1)
        {
            fprintf(stderr, "Thread %zu: WKB geometry field not found in query result\n", thread_id);
            fprintf(stderr, "Available fields: ");
            for (int i = 0; i < nfields; i++)
            {
                fprintf(stderr, "%s ", PQfname(res, i));
            }
            fprintf(stderr, "\n");
            PQclear(res);
            return false;
        }

        process_batch(res, sst, layer, layername, wkb_field_index, thread_id);

        PQclear(res);
    }

    log_progress(total_features_processed.load(), total_count, "Completed");
    fprintf(stderr, "Thread %zu: Total features processed: %zu in %zu batches (parse errors: %zu)\n",
            thread_id, total_features_processed.load(), total_batches_processed.load(), parse_errors_.load());

    return true;
}
