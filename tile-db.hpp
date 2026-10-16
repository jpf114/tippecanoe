#ifndef TILE_DB_HPP
#define TILE_DB_HPP

// MongoDB 瓦片输出专用头文件
// 用于 tippecanoe-db 程序，支持 PostGIS 输入 + MongoDB 输出

#include "tile.hpp"
#include "mongo.hpp"

// MongoDB 版本的 traverse_zooms 函数声明
// 与 tile.hpp 中的 traverse_zooms 参数一致，但内部实现使用 MongoDB 输出
int traverse_zooms(
    int *geomfd, 
    off_t *geom_size, 
    char *global_stringpool,
    std::atomic<unsigned> *midx, 
    std::atomic<unsigned> *midy,
    int &maxzoom, 
    int minzoom, 
    sqlite3 *outdb, 
    const char *outdir,
    int buffer, 
    const char *fname, 
    const char *tmpdir, 
    double gamma,
    int full_detail, 
    int low_detail, 
    int min_detail, 
    long long *pool_off,
    unsigned *initial_x, 
    unsigned *initial_y, 
    double simplification,
    double maxzoom_simplification, 
    std::vector<std::map<std::string, layermap_entry>> &layermaps,
    const char *prefilter, 
    const char *postfilter,
    std::unordered_map<std::string, attribute_op> const *attribute_accum,
    json_object *filter, 
    std::vector<strategy> &strategies, 
    int iz,
    node *shared_nodes_map, 
    size_t nodepos, 
    std::string const &shared_nodes_bloom,
    int basezoom, 
    double droprate, 
    std::vector<std::string> const &unidecode_data
);

#endif // TILE_DB_HPP
