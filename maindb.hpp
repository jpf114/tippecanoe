#ifndef MAINDB_HPP
#define MAINDB_HPP

#include "main.hpp"
#include "postgis.hpp"
#include "mongo.hpp"

// PostGIS configuration
extern bool use_postgis;
extern postgis_config postgis_cfg;

// MongoDB configuration
extern bool use_mongo;
extern mongo_config mongo_cfg;

int maindb(int argc, char **argv);

#endif // MAINDB_HPP
