#include "wkb_parser.hpp"
#include "projection.hpp"
#include "config.hpp"
#include <cstring>
#include <stdexcept>

class WKBReader {
    const uint8_t* data_;
    size_t pos_;
    size_t len_;
    bool little_endian_;
    int recursion_depth_;

    uint8_t read_byte() {
        if (pos_ >= len_) {
            throw std::runtime_error("Unexpected end of WKB data at byte " + std::to_string(pos_));
        }
        return data_[pos_++];
    }

    uint32_t read_uint32() {
        if (pos_ + 4 > len_) {
            throw std::runtime_error("Unexpected end of WKB data reading uint32 at byte " + std::to_string(pos_));
        }
        uint32_t val;
        if (little_endian_) {
            val = static_cast<uint32_t>(data_[pos_]) |
                  (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                  (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                  (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
        } else {
            val = (static_cast<uint32_t>(data_[pos_]) << 24) |
                  (static_cast<uint32_t>(data_[pos_ + 1]) << 16) |
                  (static_cast<uint32_t>(data_[pos_ + 2]) << 8) |
                  static_cast<uint32_t>(data_[pos_ + 3]);
        }
        pos_ += 4;
        return val;
    }

    double read_double() {
        if (pos_ + 8 > len_) {
            throw std::runtime_error("Unexpected end of WKB data reading double at byte " + std::to_string(pos_));
        }
        double val;
        if (little_endian_) {
            memcpy(&val, data_ + pos_, 8);
        } else {
            uint8_t buf[8];
            for (int i = 0; i < 8; i++) {
                buf[i] = data_[pos_ + 7 - i];
            }
            memcpy(&val, buf, 8);
        }
        pos_ += 8;
        return val;
    }

    void add_coord(drawvec& out, double lon, double lat, int op) {
        long long x, y;
        projection->project(lon, lat, 32, &x, &y);
        out.push_back(draw(op, x, y));
    }

    void parse_point(drawvec& out, bool has_z, bool has_m) {
        double lon = read_double();
        double lat = read_double();
        add_coord(out, lon, lat, VT_MOVETO);
        if (has_z) read_double();
        if (has_m) read_double();
    }

    void parse_linestring(drawvec& out, bool has_z, bool has_m) {
        uint32_t npoints = read_uint32();
        if (npoints > WKB_MAX_POINTS_PER_RING) {
            throw std::runtime_error("Too many points in linestring: " + std::to_string(npoints) +
                                     " (max " + std::to_string(WKB_MAX_POINTS_PER_RING) + ")");
        }
        if (npoints == 0) return;
        double lon = read_double();
        double lat = read_double();
        add_coord(out, lon, lat, VT_MOVETO);
        if (has_z) read_double();
        if (has_m) read_double();
        for (uint32_t i = 1; i < npoints; i++) {
            lon = read_double();
            lat = read_double();
            add_coord(out, lon, lat, VT_LINETO);
            if (has_z) read_double();
            if (has_m) read_double();
        }
    }

    void parse_polygon_ring(drawvec& out, bool has_z, bool has_m) {
        uint32_t npoints = read_uint32();
        if (npoints > WKB_MAX_POINTS_PER_RING) {
            throw std::runtime_error("Too many points in polygon ring: " + std::to_string(npoints) +
                                     " (max " + std::to_string(WKB_MAX_POINTS_PER_RING) + ")");
        }
        if (npoints == 0) return;
        double lon = read_double();
        double lat = read_double();
        add_coord(out, lon, lat, VT_MOVETO);
        if (has_z) read_double();
        if (has_m) read_double();
        for (uint32_t i = 1; i < npoints; i++) {
            lon = read_double();
            lat = read_double();
            add_coord(out, lon, lat, VT_LINETO);
            if (has_z) read_double();
            if (has_m) read_double();
        }
    }

    void parse_polygon(drawvec& out, bool has_z, bool has_m) {
        uint32_t nrings = read_uint32();
        if (nrings > WKB_MAX_RINGS_PER_POLYGON) {
            throw std::runtime_error("Too many rings in polygon: " + std::to_string(nrings) +
                                     " (max " + std::to_string(WKB_MAX_RINGS_PER_POLYGON) + ")");
        }
        for (uint32_t i = 0; i < nrings; i++) {
            parse_polygon_ring(out, has_z, has_m);
        }
        if (nrings > 0) {
            out.push_back(draw(VT_CLOSEPATH, 0, 0));
        }
    }

    int parse_geometry(drawvec& out, int depth = 0) {
        if (depth > WKB_MAX_RECURSION_DEPTH) {
            throw std::runtime_error("WKB recursion depth exceeded (max " +
                                     std::to_string(WKB_MAX_RECURSION_DEPTH) + ")");
        }

        uint8_t byte_order = read_byte();
        little_endian_ = (byte_order == 1);

        if (byte_order != 0 && byte_order != 1) {
            throw std::runtime_error("Invalid WKB byte order: " + std::to_string(byte_order));
        }

        uint32_t type_int = read_uint32();

        bool has_z = (type_int & WKB_Z_FLAG) != 0;
        bool has_m = (type_int & WKB_M_FLAG) != 0;
        bool has_srid = (type_int & WKB_SRID_FLAG) != 0;
        uint32_t base_type = type_int & 0xFF;

        if (has_srid) {
            read_uint32();
        }

        int geom_type = -1;

        switch (base_type) {
            case WKB_POINT:
                geom_type = GEOM_POINT;
                parse_point(out, has_z, has_m);
                break;

            case WKB_LINESTRING:
                geom_type = GEOM_LINESTRING;
                parse_linestring(out, has_z, has_m);
                break;

            case WKB_POLYGON:
                geom_type = GEOM_POLYGON;
                parse_polygon(out, has_z, has_m);
                break;

            case WKB_MULTIPOINT: {
                geom_type = GEOM_MULTIPOINT;
                uint32_t ngeoms = read_uint32();
                if (ngeoms > WKB_MAX_SUBGEOMETRIES) {
                    throw std::runtime_error("Too many sub-geometries in multipoint: " + std::to_string(ngeoms) +
                                             " (max " + std::to_string(WKB_MAX_SUBGEOMETRIES) + ")");
                }
                for (uint32_t i = 0; i < ngeoms; i++) {
                    parse_geometry(out, depth + 1);
                }
                break;
            }

            case WKB_MULTILINESTRING: {
                geom_type = GEOM_MULTILINESTRING;
                uint32_t ngeoms = read_uint32();
                if (ngeoms > WKB_MAX_SUBGEOMETRIES) {
                    throw std::runtime_error("Too many sub-geometries in multilinestring: " + std::to_string(ngeoms) +
                                             " (max " + std::to_string(WKB_MAX_SUBGEOMETRIES) + ")");
                }
                for (uint32_t i = 0; i < ngeoms; i++) {
                    parse_geometry(out, depth + 1);
                }
                break;
            }

            case WKB_MULTIPOLYGON: {
                geom_type = GEOM_MULTIPOLYGON;
                uint32_t ngeoms = read_uint32();
                if (ngeoms > WKB_MAX_SUBGEOMETRIES) {
                    throw std::runtime_error("Too many sub-geometries in multipolygon: " + std::to_string(ngeoms) +
                                             " (max " + std::to_string(WKB_MAX_SUBGEOMETRIES) + ")");
                }
                for (uint32_t i = 0; i < ngeoms; i++) {
                    parse_geometry(out, depth + 1);
                }
                break;
            }

            case WKB_GEOMETRYCOLLECTION: {
                geom_type = GEOM_TYPES;
                uint32_t ngeoms = read_uint32();
                if (ngeoms > WKB_MAX_SUBGEOMETRIES) {
                    throw std::runtime_error("Too many sub-geometries in collection: " + std::to_string(ngeoms) +
                                             " (max " + std::to_string(WKB_MAX_SUBGEOMETRIES) + ")");
                }
                for (uint32_t i = 0; i < ngeoms; i++) {
                    parse_geometry(out, depth + 1);
                }
                break;
            }

            default:
                throw std::runtime_error("Unknown WKB geometry type: " + std::to_string(base_type));
        }

        return geom_type;
    }

public:
    WKBResult parse(const uint8_t* data, size_t len) {
        data_ = data;
        pos_ = 0;
        len_ = len;
        little_endian_ = false;
        recursion_depth_ = 0;

        WKBResult result;
        result.geometry_type = -1;
        result.valid = false;
        result.srid = 0;
        result.has_z = false;
        result.has_m = false;

        try {
            if (len == 0) {
                result.error = "Empty WKB data";
                return result;
            }

            result.geometry_type = parse_geometry(result.coordinates);
            result.valid = true;

        } catch (const std::exception& e) {
            result.error = std::string("WKB parse error at byte ") +
                          std::to_string(pos_) + ": " + e.what();
            DEBUG_LOG("WKB parse failed: %s", result.error.c_str());
        } catch (...) {
            result.error = std::string("WKB parse error at byte ") +
                          std::to_string(pos_) + ": unknown exception";
            DEBUG_LOG("WKB parse failed: %s", result.error.c_str());
        }

        return result;
    }
};

WKBResult parse_wkb(const uint8_t* data, size_t len) {
    WKBReader reader;
    return reader.parse(data, len);
}

static uint8_t hex_char_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

WKBResult parse_wkb_hex(const std::string& hex) {
    if (hex.empty()) {
        WKBResult result;
        result.geometry_type = -1;
        result.valid = false;
        result.error = "Empty hex string";
        return result;
    }

    if (hex.size() < 2 || hex.size() % 2 != 0) {
        WKBResult result;
        result.geometry_type = -1;
        result.valid = false;
        result.error = "Invalid hex string length: " + std::to_string(hex.size());
        return result;
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);

    for (size_t i = 0; i < hex.size(); i += 2) {
        uint8_t hi = hex_char_val(hex[i]);
        uint8_t lo = hex_char_val(hex[i + 1]);
        bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }

    return parse_wkb(bytes.data(), bytes.size());
}
