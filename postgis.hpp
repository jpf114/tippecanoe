#ifndef POSTGIS_HPP
#define POSTGIS_HPP

#include <string>
#include <vector>
#include "geometry.hpp"
#include "serial.hpp"

struct postgis_config
{
    std::string host;
    std::string port;
    std::string dbname;
    std::string user;
    std::string password;
    std::string table;
    std::string geometry_field;
    std::string sql;

    postgis_config()
        : host("localhost"),
          port("5432"),
          dbname(""),
          user(""),
          password(""),
          table(""),
          geometry_field("geometry"),
          sql("")
    {
    }
};

class PostGISReader
{
public:
    PostGISReader(const postgis_config &cfg);
    ~PostGISReader();

    bool connect();
    bool read_features(std::vector<struct serialization_state> &sst, size_t layer, const std::string &layername);

protected:
    bool execute_query(const std::string &query);
    bool parse_geometry(const std::string &wkt, struct serial_feature &f);

private:
    postgis_config config;
    void *conn;
    key_pool kpool;
};

#endif // POSTGIS_HPP