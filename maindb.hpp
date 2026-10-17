#ifndef MAINDB_HPP
#define MAINDB_HPP

#include "main.hpp"
#include "postgis.hpp"
#include "mongo.hpp"

extern bool use_postgis;
extern postgis_config postgis_cfg;

extern bool use_mongo;
extern mongo_config mongo_cfg;

// v3.4 (副本收敛): 注入 MongoDB 附加写出 sink（实现见 tile-db.cpp 薄壳）；
// 必须在 read_input（启动瓦片工作线程）之前调用。
void set_tile_db_output_sinks();
extern mongo_config mongo_cfg;

int maindb(int argc, char **argv);

#endif // MAINDB_HPP
