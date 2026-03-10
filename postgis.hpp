#ifndef POSTGIS_HPP
#define POSTGIS_HPP

#include <string>
#include <vector>
#include "geometry.hpp"

struct postgis_config {
    std::string host;
    std::string port;
    std::string dbname;
    std::string user;
    std::string password;
    std::string table;
    std::string geometry_column;
    std::string where_clause;
};

class PostGISReader {
public:
    PostGISReader(const postgis_config &config);
    ~PostGISReader();
    
    bool connect();
    bool read_features(std::vector<struct serialization_state> &sst, size_t layer, const std::string &layername);
    
private:
    postgis_config config;
    void *conn;
    
    bool execute_query(const std::string &query);
    bool parse_geometry(const std::string &wkt, struct feature &f);
};

#endif // POSTGIS_HPP
