#include "postgis.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <libpq-fe.h>
#include "geometry.hpp"
#include "serial.hpp"

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
        query = "SELECT ST_AsText(" + config.geometry_field + ") as wkt, * FROM " + config.table;
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

    if (nfields < 2)
    {
        fprintf(stderr, "Error: Query must return at least 2 columns (geometry + at least one attribute)\n");
        PQclear(res);
        return false;
    }

    int geom_field_index = -1;

    // First, try to find the geometry column by name
    if (!config.geometry_field.empty())
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

    // If not found, try to find a column named 'wkt' (common for ST_AsText output)
    if (geom_field_index == -1)
    {
        for (int i = 0; i < nfields; i++)
        {
            if (strcmp(PQfname(res, i), "wkt") == 0)
            {
                geom_field_index = i;
                break;
            }
        }
    }

    // If still not found, use the second column as fallback
    if (geom_field_index == -1)
    {
        fprintf(stderr, "Warning: Geometry column not found, using second column (index 1)\n");
        geom_field_index = 1;
    }

    const char *geom_field_name = PQfname(res, geom_field_index);
    if (!geom_field_name || strlen(geom_field_name) == 0)
    {
        fprintf(stderr, "Using geometry column: %s (index %d)\n", geom_field_name, geom_field_index);
        fprintf(stderr, "Error: Geometry column name is exist or empty\n");
        return false;
    }

    for (int i = 0; i < ntuples; i++)
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

        for (int field = 0; field < nfields; field++)
        {
            char *value = PQgetvalue(res, i, field);
            const char *fieldname = PQfname(res, field);

            if (value == NULL || strlen(value) == 0)
            {
                continue;
            }

            if (field == geom_field_index)
            {
                if (!parse_geometry(std::string(value), f))
                {
                    continue;
                }
            }
            else
            {
                serial_val sv;
                Oid type = PQftype(res, field);

                if (type == 20 || type == 21 || type == 23 || type == 700 || type == 701)
                {
                    sv.type = mvt_double;
                    sv.s = milo::dtoa_milo(atof(value));
                }
                else if (type == 16)
                {
                    sv.type = mvt_bool;
                    sv.s = (strcmp(value, "t") == 0) ? "true" : "false";
                }
                else
                {
                    sv.type = mvt_string;
                    sv.s = value;
                }

                full_keys.push_back(kpool.pool(fieldname));
                full_values.push_back(sv);
            }
        }

        f.full_keys = full_keys;
        f.full_values = full_values;

        if (f.t != VT_UNKNOWN && f.geometry.size() > 0)
        {
            serialize_feature(&sst[0], f, config.table.empty() ? "postgis" : config.table);
        }
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

bool PostGISReader::parse_geometry(const std::string &wkt, struct serial_feature &f)
{
    drawvec dv;

    if (wkt.substr(0, 5) == "POINT")
    {
        f.t = VT_POINT;
        size_t start = wkt.find('(') + 1;
        size_t end = wkt.find(')');
        if (start != std::string::npos && end != std::string::npos)
        {
            std::string coords = wkt.substr(start, end - start);
            double x, y;
            if (sscanf(coords.c_str(), "%lf %lf", &x, &y) == 2)
            {
                draw d;
                d.op = VT_MOVETO;
                d.x = (long long)(x * (1LL << 32) / 360.0 + (1LL << 31));
                d.y = (long long)((90.0 - y) * (1LL << 32) / 180.0);
                dv.push_back(d);
            }
        }
    }
    else if (wkt.substr(0, 10) == "LINESTRING")
    {
        f.t = VT_LINE;
        size_t start = wkt.find('(') + 1;
        size_t end = wkt.find(')');
        if (start != std::string::npos && end != std::string::npos)
        {
            std::string coords = wkt.substr(start, end - start);
            bool first = true;
            char *coord_str = strdup(coords.c_str());
            char *token = strtok(coord_str, ",");

            while (token != NULL)
            {
                double x, y;
                if (sscanf(token, "%lf %lf", &x, &y) == 2)
                {
                    draw d;
                    d.op = first ? VT_MOVETO : VT_LINETO;
                    d.x = (long long)(x * (1LL << 32) / 360.0 + (1LL << 31));
                    d.y = (long long)((90.0 - y) * (1LL << 32) / 180.0);
                    dv.push_back(d);
                    first = false;
                }
                token = strtok(NULL, ",");
            }
            free(coord_str);
        }
    }
    else if (wkt.substr(0, 7) == "POLYGON")
    {
        f.t = VT_POLYGON;
        size_t start = wkt.find('(') + 1;
        size_t end = wkt.rfind(')');
        if (start != std::string::npos && end != std::string::npos)
        {
            std::string coords = wkt.substr(start, end - start);
            char *coord_str = strdup(coords.c_str());
            char *outer = strtok(coord_str, ")");

            if (outer != NULL)
            {
                bool first = true;
                char *token = strtok(outer + 1, ",");

                while (token != NULL)
                {
                    double x, y;
                    if (sscanf(token, "%lf %lf", &x, &y) == 2)
                    {
                        draw d;
                        d.op = first ? VT_MOVETO : VT_LINETO;
                        d.x = (long long)(x * (1LL << 32) / 360.0 + (1LL << 31));
                        d.y = (long long)((90.0 - y) * (1LL << 32) / 180.0);
                        dv.push_back(d);
                        first = false;
                    }
                    token = strtok(NULL, ",");
                }

                if (dv.size() > 0)
                {
                    dv.push_back(dv[0]);
                }
            }
            free(coord_str);
        }
    }
    else
    {
        return false;
    }

    f.geometry = dv;
    return dv.size() > 0;
}