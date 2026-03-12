#include "postgis.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <libpq-fe.h>
#include <string.h>
#include "geometry.hpp"
#include "serial.hpp"
#include "geojson.hpp"

PostGISReader::PostGISReader(const postgis_config &cfg) : config(cfg), conn(NULL)
{
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
    const char *keywords[] = {"host", "port", "dbname", "user", "password", NULL};
    const char *values[] = {config.host.c_str(), config.port.c_str(), config.dbname.c_str(), config.user.c_str(), config.password.c_str(), NULL};
    conn = PQconnectdbParams(keywords, values, 0);

    if (PQstatus((PGconn *)conn) != CONNECTION_OK)
    {
        fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage((PGconn *)conn));
        return false;
    }

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

    std::string query;
    if (!config.sql.empty())
    {
        query = config.sql;
    }
    else if (!config.table.empty() && !config.geometry_field.empty())
    {
        // Generate SQL query from table and geometry_field
        query = "SELECT ST_AsGeoJSON(" + config.geometry_field + ") as geojson, * FROM " + config.table;
        // Replace the original geometry column with WKT format
        // query = "SELECT ST_AsGeoJSON(" + config.geometry_field + ") as geojson, ST_AsText(" + config.geometry_field + ") as " + config.geometry_field + ", * FROM " + config.table;
    }
    else
    {
        fprintf(stderr, "Error: Either --postgis-sql or both --postgis-table and --postgis-geometry-field are required for PostGIS input\n");
        return false;
    }

    PGresult *res = PQexec((PGconn *)conn, query.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        fprintf(stderr, "Query failed: %s\n", PQerrorMessage((PGconn *)conn));
        PQclear(res);
        return false;
    }

    int ntuples = PQntuples(res);
    int nfields = PQnfields(res);

    if (nfields < 1)
    {
        fprintf(stderr, "Error: Query must return at least 1 column (geometry)\n");
        PQclear(res);
        return false;
    }

    // Collect all features to build a single GeoJSON FeatureCollection
    std::vector<std::string> features;

    int geom_field_index = -1;

    // First, try to find a column named 'geojson' (common for ST_AsGeoJSON output)
    for (int i = 0; i < nfields; i++)
    {
        if (strcmp(PQfname(res, i), "geojson") == 0)
        {
            geom_field_index = i;
            break;
        }
    }

    // If not found, try to find the geometry column by name
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

    // If still not found, use the first column as fallback
    if (geom_field_index == -1)
    {
        fprintf(stderr, "Warning: Geometry column not found, using first column (index 0)\n");
        geom_field_index = 0;
    }

    const char *geom_field_name = PQfname(res, geom_field_index);
    if (!geom_field_name || strlen(geom_field_name) == 0)
    {
        fprintf(stderr, "Error: Geometry column name does not exist or is empty (index %d)\n", geom_field_index);
        PQclear(res);
        return false;
    }
    fprintf(stderr, "Using geometry column: %s (index %d)\n", geom_field_name, geom_field_index);

    for (int i = 0; i < ntuples; i++)
    {
        // Build GeoJSON feature string
        std::string feature_str = "{";
        bool has_properties = false;

        // First pass: check if there are any properties other than geometry
        for (int field = 0; field < nfields; field++)
        {
            if (field != geom_field_index)
            {
                char *value = PQgetvalue(res, i, field);
                if (value != NULL && strlen(value) > 0)
                {
                    has_properties = true;
                    break;
                }
            }
        }

        // Add geometry and type
        char *geom_value = PQgetvalue(res, i, geom_field_index);
        if (geom_value != NULL && strlen(geom_value) > 0)
        {
            feature_str += "\"type\":\"Feature\",";
            feature_str += "\"geometry\":" + std::string(geom_value);

            // Add properties if there are any
            if (has_properties)
            {
                feature_str += ",\"properties\":{";
                bool first_property = true;

                for (int field = 0; field < nfields; field++)
                {
                    if (field != geom_field_index)
                    {
                        const char *fieldname = PQfname(res, field);
                        if (fieldname == NULL || strlen(fieldname) == 0 || strcmp(fieldname, config.geometry_field.c_str()) == 0)
                        {
                            continue;
                        }

                        char *value = PQgetvalue(res, i, field);
                        if (value == NULL || strlen(value) == 0)
                        {
                            continue;
                        }
                        if (!first_property)
                        {
                            feature_str += ",";
                        }
                        first_property = false;

                        feature_str += "\"" + std::string(fieldname) + "\":";
                        Oid type = PQftype(res, field);

                        if (type == 20 || type == 21 || type == 23 || type == 700 || type == 701)
                        {
                            // Numeric type
                            feature_str += std::string(value);
                        }
                        else if (type == 16)
                        {
                            // Boolean type
                            feature_str += (strcmp(value, "t") == 0) ? "true" : "false";
                        }
                        else
                        {
                            // String type - need to escape
                            std::string escaped_value = std::string(value);
                            size_t pos = 0;
                            while ((pos = escaped_value.find('"', pos)) != std::string::npos)
                            {
                                escaped_value.replace(pos, 1, "\\\"");
                                pos += 2;
                            }
                            // Escape backslashes as well
                            pos = 0;
                            while ((pos = escaped_value.find('\\', pos)) != std::string::npos)
                            {
                                escaped_value.replace(pos, 1, "\\\\");
                                pos += 2;
                            }
                            feature_str += "\"" + escaped_value + "\"";
                        }
                    }
                }
                feature_str += "}";
            }
            feature_str += "}";
            features.push_back(feature_str);
        }
    }

    // Build GeoJSON FeatureCollection
    if (!features.empty())
    {
        std::string geojson_collection = "{\"type\":\"FeatureCollection\",\"features\":[";
        for (size_t j = 0; j < features.size(); j++)
        {
            if (j > 0)
            {
                geojson_collection += ",";
            }
            geojson_collection += features[j];
        }
        geojson_collection += "]}";

        // Parse the GeoJSON FeatureCollection using existing GeoJSON parsing logic
        char *geojson_buffer = strdup(geojson_collection.c_str());
        if (geojson_buffer)
        {
            long long geojson_len = geojson_collection.length();

            struct json_pull *jp = json_begin_map(geojson_buffer, geojson_len);
            if (jp != NULL)
            {
                // Create a temporary serialization_state for parsing
                struct serialization_state temp_sst = sst[0];
                temp_sst.fname = "PostGIS";
                temp_sst.line = 0;

                parse_json(&temp_sst, jp, layer, layername);
                json_end_map(jp);
            }

            free(geojson_buffer);
        }
    }

    PQclear(res);
    return true;
}

//
bool PostGISReader::execute_query(const std::string &query)
{
    if (!conn)
    {
        fprintf(stderr, "Error: Not connected to database\n");
        return false;
    }

    PGresult *res = PQexec((PGconn *)conn, query.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        fprintf(stderr, "Query failed: %s\n", PQerrorMessage((PGconn *)conn));
        PQclear(res);
        return false;
    }

    PQclear(res);
    return true;
}
