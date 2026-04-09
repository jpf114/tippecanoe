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
#endif

extern const int mb_geometry[GEOM_TYPES];

struct WKBResult {
    int geometry_type;
    drawvec coordinates;
    bool valid;
    std::string error;
    int srid;
    bool has_z;
    bool has_m;
};

WKBResult parse_wkb(const uint8_t* data, size_t len);
WKBResult parse_wkb_hex(const std::string& hex);

#endif
