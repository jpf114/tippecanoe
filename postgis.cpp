#include "postgis.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <libpq-fe.h>
#include "geometry.hpp"
#include "serial.hpp"

PostGISReader::PostGISReader(const postgis_config &config) : config(config), conn(NULL) {
}

PostGISReader::~PostGISReader() {
    if (conn) {
        PQfinish((PGconn *)conn);
    }
}

bool PostGISReader::connect() {
    std::string conninfo = "host='" + config.host + "' port='" + config.port + "' dbname='" + config.dbname + "' user='" + config.user + "' password='" + config.password + "'";
    conn = PQconnectdb(conninfo.c_str());
    
    if (PQstatus((PGconn *)conn) != CONNECTION_OK) {
        fprintf(stderr, "Connection to database failed: %s\n", PQerrorMessage((PGconn *)conn));
        return false;
    }
    
    return true;
}

bool PostGISReader::execute_query(const std::string &query) {
    PGresult *res = PQexec((PGconn *)conn, query.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "Query failed: %s\n", PQerrorMessage((PGconn *)conn));
        PQclear(res);
        return false;
    }
    
    PQclear(res);
    return true;
}

bool PostGISReader::parse_geometry(const std::string &wkt, struct feature &f) {
    // This is a simplified WKT parser - in a real implementation, you would use a proper WKT parser
    // or use PostGIS's ST_AsGeoJSON function to get GeoJSON directly
    if (wkt.substr(0, 5) == "POINT") {
        f.type = VT_POINT;
        // Parse point coordinates
    } else if (wkt.substr(0, 10) == "LINESTRING") {
        f.type = VT_LINE;
        // Parse linestring coordinates
    } else if (wkt.substr(0, 7) == "POLYGON") {
        f.type = VT_POLYGON;
        // Parse polygon coordinates
    }
    return true;
}

bool PostGISReader::read_features(std::vector<struct serialization_state> &sst, size_t layer, const std::string &layername) {
    if (!conn) {
        if (!connect()) {
            return false;
        }
    }
    
    std::string query = "SELECT " + config.geometry_column + ", ST_AsText(" + config.geometry_column + ") as wkt";
    query += " FROM " + config.table;
    if (!config.where_clause.empty()) {
        query += " WHERE " + config.where_clause;
    }
    
    PGresult *res = PQexec((PGconn *)conn, query.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        fprintf(stderr, "Query failed: %s\n", PQerrorMessage((PGconn *)conn));
        PQclear(res);
        return false;
    }
    
    int ntuples = PQntuples(res);
    for (int i = 0; i < ntuples; i++) {
        // Get WKT geometry
        char *wkt = PQgetvalue(res, i, 1);
        if (wkt) {
            struct feature f;
            f.layer = layer;
            f.seq = i;
            f.t = VT_UNKNOWN;
            
            if (parse_geometry(std::string(wkt), f)) {
                // Serialize feature
                for (size_t j = 0; j < sst.size(); j++) {
                    // Add feature to each serialization state
                    // This is a simplified implementation - in a real implementation, you would
                    // properly serialize the feature to each reader's geometry file
                }
            }
        }
    }
    
    PQclear(res);
    return true;
}
