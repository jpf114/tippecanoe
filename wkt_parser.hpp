#ifndef WKT_PARSER_HPP
#define WKT_PARSER_HPP

#include "geometry.hpp"
#include <string>

// Geometry type constants (same as read_json.hpp)
#define GEOM_POINT 0	       /* array of positions */
#define GEOM_MULTIPOINT 1      /* array of arrays of positions */
#define GEOM_LINESTRING 2      /* array of arrays of positions */
#define GEOM_MULTILINESTRING 3 /* array of arrays of arrays of positions */
#define GEOM_POLYGON 4	       /* array of arrays of arrays of positions */
#define GEOM_MULTIPOLYGON 5    /* array of arrays of arrays of arrays of positions */
#define GEOM_TYPES 6

// mb_geometry is defined in read_json.cpp
extern const int mb_geometry[GEOM_TYPES];

/**
 * @brief WKT 解析结果结构
 */
struct WKTResult {
    int geometry_type;      // GEOM_POINT, GEOM_LINESTRING, etc.
    drawvec coordinates;    // 解析后的坐标向量
    bool valid;             // 解析是否成功
    std::string error;      // 错误信息（如果有）
};

/**
 * @brief 解析 WKT 字符串为 drawvec
 * 
 * 支持以下 WKT 几何类型：
 * - POINT
 * - LINESTRING
 * - POLYGON
 * - MULTIPOINT
 * - MULTILINESTRING
 * - MULTIPOLYGON
 * 
 * @param wkt WKT 格式的几何字符串
 * @return WKTResult 解析结果
 */
WKTResult parse_wkt(const std::string& wkt);

#endif // WKT_PARSER_HPP
