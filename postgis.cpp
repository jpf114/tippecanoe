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
    std::string conninfo = "host='" + config.host + "' port='" + config.port + "' dbname='" + config.dbname + "' user='" + config.user + "' password='" + config.password + "'";
    conn = PQconnectdb(conninfo.c_str());

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
        return false;
    }
    fprintf(stderr, "Using geometry column: %s (index %d)\n", geom_field_name, geom_field_index);

    for (int i = 0; i < 3/*ntuples*/; i++)
    {
        struct serial_feature f;
        f.layer = layer;
        f.seq = i;
        f.has_id = false;
        f.id = 0;
        f.tippecanoe_minzoom = -1;
        f.tippecanoe_maxzoom = -1;
        f.feature_minzoom = 0;

        std::vector<std::shared_ptr<std::string>> full_keys;
        std::vector<serial_val> full_values;

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
                        char *value = PQgetvalue(res, i, field);
                        const char *fieldname = PQfname(res, field);

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

        fprintf(stderr, "GeoJSON FeatureCollection: %s\n", geojson_collection.c_str());
        
        // Parse the GeoJSON FeatureCollection using existing GeoJSON parsing logic
        char *geojson_buffer = strdup(geojson_collection.c_str());
        long long geojson_len = geojson_collection.length();

        struct json_pull *jp = json_begin_map(geojson_buffer, geojson_len);
        if (jp != NULL)
        {
            // Create a temporary serialization_state for parsing
            struct serialization_state temp_sst = sst[0];
            temp_sst.fname = "PostGIS";
            temp_sst.line = 0;
            
            parse_json(&temp_sst, jp, layer, config.table.empty() ? "postgis" : config.table);
            json_end_map(jp);
        }

        free(geojson_buffer);
    }

    PQclear(res);
    return true;
}

//
bool PostGISReader::execute_query(const std::string &query)
{
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

