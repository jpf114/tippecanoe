#ifndef WKB_PARSER_HPP
#define WKB_PARSER_HPP

#include "geometry.hpp"
#include <string>
#include <cstdint>
#include <vector>

#define WKB_POINT              1
#define WKB_LINESTRING         2
#define WKB_POLYGON            3
#define WKB_MULTIPOINT         4
#define WKB_MULTILINESTRING    5
#define WKB_MULTIPOLYGON       6
#define WKB_GEOMETRYCOLLECTION 7

#define WKB_SRID_FLAG 0x20000000
#define WKB_Z_FLAG    0x80000000
#define WKB_M_FLAG    0x40000000

#ifndef GEOM_POINT
#define GEOM_POINT 0
#define GEOM_MULTIPOINT 1
#define GEOM_LINESTRING 2
#define GEOM_MULTILINESTRING 3
#define GEOM_POLYGON 4
#define GEOM_MULTIPOLYGON 5
#define GEOM_TYPES 6
#define GEOM_GEOMETRYCOLLECTION 6
#endif

constexpr uint32_t WKB_MAX_POINTS_PER_RING = 10000000;
constexpr uint32_t WKB_MAX_RINGS_PER_POLYGON = 100000;
constexpr uint32_t WKB_MAX_SUBGEOMETRIES = 100000;
constexpr int WKB_MAX_RECURSION_DEPTH = 16;

extern const int mb_geometry[GEOM_TYPES];

struct WKBResult {
    int geometry_type = -1;
    drawvec coordinates;
    bool valid;
    std::string error;
    int srid;
    bool has_z;
    bool has_m;
    // v3.4 (DB-S4): GEOMETRYCOLLECTION 的子几何（嵌套 collection 已递归摊平）。
    // 非 collection 类型该字段为空；collection 自身的 coordinates 为空，
    // 每个子几何一个 WKBResult，由调用方逐个产出 feature（对齐原版
    // GeoJSON 路径把 collection 拆成多个 feature 的语义）。
    std::vector<WKBResult> parts;
};

WKBResult parse_wkb(const uint8_t* data, size_t len);
WKBResult parse_wkb_hex(const std::string& hex);

#endif
