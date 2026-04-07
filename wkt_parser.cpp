#include "wkt_parser.hpp"
#include "projection.hpp"
#include "config.hpp"
#include <cstring>
#include <cctype>
#include <stdexcept>
#include <vector>

class WKTParser {
    const char* data;
    size_t pos;
    size_t len;
    
    void skip_whitespace() {
        while (pos < len && std::isspace(data[pos])) pos++;
    }
    
    char peek() {
        return (pos >= len) ? '\0' : data[pos];
    }
    
    char advance() {
        return (pos >= len) ? '\0' : data[pos++];
    }
    
    bool match(char c) {
        skip_whitespace();
        if (peek() == c) { 
            pos++; 
            return true; 
        }
        return false;
    }
    
    std::string read_word() {
        skip_whitespace();
        std::string result;
        while (pos < len && std::isalpha(data[pos])) {
            result += data[pos++];
        }
        return result;
    }
    
    double read_number() {
        skip_whitespace();
        size_t start = pos;
        
        if (pos >= len) {
            throw std::runtime_error("Unexpected end of input while reading number");
        }
        
        if (data[pos] == '-') pos++;
        
        while (pos < len && (std::isdigit(data[pos]) || 
               data[pos] == '.' || 
               data[pos] == 'e' || data[pos] == 'E' || 
               data[pos] == '+' || data[pos] == '-')) {
            if ((data[pos] == '+' || data[pos] == '-') && 
                pos > start && data[pos-1] != 'e' && data[pos-1] != 'E') {
                break;
            }
            pos++;
        }
        
        if (pos == start) {
            throw std::runtime_error("Expected number at position " + std::to_string(start));
        }
        
        std::string num_str(data + start, pos - start);
        try {
            return std::stod(num_str);
        } catch (const std::exception& e) {
            throw std::runtime_error("Invalid number format: '" + num_str + "'");
        }
    }
    
    std::pair<double, double> read_coordinate() {
        double lon = read_number();
        double lat = read_number();
        return {lon, lat};
    }
    
    void add_coordinate(drawvec& out, double lon, double lat, int op) {
        long long x, y;
        projection->project(lon, lat, 32, &x, &y);
        out.push_back(draw(op, x, y));
    }

public:
    WKTResult parse(const std::string& wkt) {
        data = wkt.c_str();
        len = wkt.length();
        pos = 0;
        
        WKTResult result;
        result.valid = false;
        
        try {
            if (len == 0) {
                result.error = "Empty WKT string";
                return result;
            }
            
            skip_whitespace();
            
            DEBUG_LOG("Parsing WKT (length=%zu): %.50s%s", len, wkt.substr(0, 50).c_str(), 
                     len > 50 ? "..." : "");
            
            std::string type = read_word();
            if (type.empty()) {
                result.error = "No geometry type found in WKT";
                return result;
            }
            
            // Handle Z/M dimension markers
            skip_whitespace();
            while (pos < len && std::isalpha(data[pos])) {
                char dim = data[pos++];
                if (dim == 'Z' || dim == 'M') {
                    // Ignore Z/M dimensions
                }
            }
            
            if (type == "POINT") {
                result.geometry_type = GEOM_POINT;
                if (!parse_point(result.coordinates)) {
                    result.error = "Failed to parse POINT geometry";
                    return result;
                }
                result.valid = true;
            } else if (type == "LINESTRING") {
                result.geometry_type = GEOM_LINESTRING;
                if (!parse_linestring(result.coordinates)) {
                    result.error = "Failed to parse LINESTRING geometry";
                    return result;
                }
                result.valid = true;
            } else if (type == "POLYGON") {
                result.geometry_type = GEOM_POLYGON;
                if (!parse_polygon(result.coordinates)) {
                    result.error = "Failed to parse POLYGON geometry";
                    return result;
                }
                result.valid = true;
            } else if (type == "MULTIPOINT") {
                result.geometry_type = GEOM_MULTIPOINT;
                if (!parse_multipoint(result.coordinates)) {
                    result.error = "Failed to parse MULTIPOINT geometry";
                    return result;
                }
                result.valid = true;
            } else if (type == "MULTILINESTRING") {
                result.geometry_type = GEOM_MULTILINESTRING;
                if (!parse_multilinestring(result.coordinates)) {
                    result.error = "Failed to parse MULTILINESTRING geometry";
                    return result;
                }
                result.valid = true;
            } else if (type == "MULTIPOLYGON") {
                result.geometry_type = GEOM_MULTIPOLYGON;
                if (!parse_multipolygon(result.coordinates)) {
                    result.error = "Failed to parse MULTIPOLYGON geometry";
                    return result;
                }
                result.valid = true;
            } else if (type == "GEOMETRYCOLLECTION") {
                result.geometry_type = GEOM_TYPES;  // Special marker for collection
                if (!parse_geometrycollection(result.coordinates)) {
                    result.error = "Failed to parse GEOMETRYCOLLECTION geometry";
                    return result;
                }
                result.valid = true;
            } else {
                result.error = "Unknown geometry type: " + type;
                return result;
            }
            
        } catch (const std::exception& e) {
            result.error = std::string("Parse error at position ") + 
                          std::to_string(pos) + ": " + e.what();
            DEBUG_LOG("WKT parse failed: %s", result.error.c_str());
        }
        
        return result;
    }
    
    bool parse_point(drawvec& out) {
        if (!match('(')) {
            throw std::runtime_error("Expected '(' after POINT");
        }
        
        auto coord = read_coordinate();
        add_coordinate(out, coord.first, coord.second, VT_MOVETO);
        
        // Skip Z coordinate if present
        skip_whitespace();
        if (peek() != ')' && (std::isdigit(peek()) || peek() == '-')) {
            read_number();
        }
        
        skip_whitespace();
        if (!match(')')) {
            throw std::runtime_error("Expected ')' at end of POINT");
        }
        
        return true;
    }
    
    bool parse_linestring(drawvec& out) {
        if (!match('(')) {
            throw std::runtime_error("Expected '(' after LINESTRING");
        }
        
        bool first = true;
        while (peek() != ')') {
            auto coord = read_coordinate();
            add_coordinate(out, coord.first, coord.second, 
                          first ? VT_MOVETO : VT_LINETO);
            first = false;
            
            // Skip Z coordinate if present
            skip_whitespace();
            if (peek() != ',' && peek() != ')' && (std::isdigit(peek()) || peek() == '-')) {
                read_number();
            }
            
            skip_whitespace();
            if (peek() == ',') advance();
        }
        
        if (!match(')')) {
            throw std::runtime_error("Expected ')' at end of LINESTRING");
        }
        return true;
    }
    
    bool parse_polygon(drawvec& out) {
        if (!match('(')) {
            throw std::runtime_error("Expected '(' after POLYGON");
        }
        
        while (peek() != ')') {
            if (!match('(')) {
                throw std::runtime_error("Expected '(' for ring in POLYGON");
            }
            
            bool first = true;
            while (peek() != ')') {
                auto coord = read_coordinate();
                add_coordinate(out, coord.first, coord.second,
                              first ? VT_MOVETO : VT_LINETO);
                first = false;
                
                // Skip Z coordinate if present
                skip_whitespace();
                if (peek() != ',' && peek() != ')' && (std::isdigit(peek()) || peek() == '-')) {
                    read_number();
                }
                
                skip_whitespace();
                if (peek() == ',') advance();
            }
            
            if (!match(')')) {
                throw std::runtime_error("Expected ')' at end of ring");
            }
            skip_whitespace();
            if (peek() == ',') advance();
        }
        
        if (!match(')')) {
            throw std::runtime_error("Expected ')' at end of POLYGON");
        }
        return true;
    }
    
    bool parse_multipoint(drawvec& out) {
        if (!match('(')) {
            throw std::runtime_error("Expected '(' after MULTIPOINT");
        }
        
        while (peek() != ')') {
            bool has_parens = match('(');
            auto coord = read_coordinate();
            add_coordinate(out, coord.first, coord.second, VT_MOVETO);
            
            // Skip Z coordinate if present
            if (has_parens) {
                skip_whitespace();
                if (peek() != ')' && (std::isdigit(peek()) || peek() == '-')) {
                    read_number();
                }
            }
            
            if (has_parens) {
                if (!match(')')) {
                    throw std::runtime_error("Expected ')' in MULTIPOINT element");
                }
            }
            
            skip_whitespace();
            if (peek() == ',') advance();
        }
        
        if (!match(')')) {
            throw std::runtime_error("Expected ')' at end of MULTIPOINT");
        }
        return true;
    }
    
    bool parse_multilinestring(drawvec& out) {
        if (!match('(')) {
            throw std::runtime_error("Expected '(' after MULTILINESTRING");
        }
        
        while (peek() != ')') {
            if (!match('(')) {
                throw std::runtime_error("Expected '(' for linestring in MULTILINESTRING");
            }
            if (!parse_linestring(out)) {
                return false;
            }
            if (!match(')')) {
                throw std::runtime_error("Expected ')' at end of linestring in MULTILINESTRING");
            }
            
            skip_whitespace();
            if (peek() == ',') advance();
        }
        
        if (!match(')')) {
            throw std::runtime_error("Expected ')' at end of MULTILINESTRING");
        }
        return true;
    }
    
    bool parse_multipolygon(drawvec& out) {
        if (!match('(')) {
            throw std::runtime_error("Expected '(' after MULTIPOLYGON");
        }
        
        while (peek() != ')') {
            if (!match('(')) {
                throw std::runtime_error("Expected '(' for polygon in MULTIPOLYGON");
            }
            
            while (peek() != ')') {
                if (!match('(')) {
                    throw std::runtime_error("Expected '(' for ring in MULTIPOLYGON");
                }
                
                bool first = true;
                while (peek() != ')') {
                    auto coord = read_coordinate();
                    add_coordinate(out, coord.first, coord.second,
                                  first ? VT_MOVETO : VT_LINETO);
                    first = false;
                    
                    // Skip Z coordinate if present
                    skip_whitespace();
                    if (peek() != ',' && peek() != ')' && (std::isdigit(peek()) || peek() == '-')) {
                        read_number();
                    }
                    
                    skip_whitespace();
                    if (peek() == ',') advance();
                }
                
                if (!match(')')) {
                    throw std::runtime_error("Expected ')' at end of ring in MULTIPOLYGON");
                }
                skip_whitespace();
                if (peek() == ',') advance();
            }
            
            if (!match(')')) {
                throw std::runtime_error("Expected ')' at end of polygon in MULTIPOLYGON");
            }
            
            skip_whitespace();
            if (peek() == ',') advance();
        }
        
        if (!match(')')) {
            throw std::runtime_error("Expected ')' at end of MULTIPOLYGON");
        }
        return true;
    }
    
    bool parse_geometrycollection(drawvec& out) {
        if (!match('(')) {
            throw std::runtime_error("Expected '(' after GEOMETRYCOLLECTION");
        }
        
        while (peek() != ')') {
            std::string subtype = read_word();
            if (subtype.empty()) {
                throw std::runtime_error("Expected geometry type in GEOMETRYCOLLECTION");
            }
            
            // Handle Z/M dimension markers
            skip_whitespace();
            while (pos < len && std::isalpha(data[pos])) {
                pos++;  // Skip Z/M markers
            }
            
            bool success = false;
            if (subtype == "POINT") {
                success = parse_point(out);
            } else if (subtype == "LINESTRING") {
                success = parse_linestring(out);
            } else if (subtype == "POLYGON") {
                success = parse_polygon(out);
            } else if (subtype == "MULTIPOINT") {
                success = parse_multipoint(out);
            } else if (subtype == "MULTILINESTRING") {
                success = parse_multilinestring(out);
            } else if (subtype == "MULTIPOLYGON") {
                success = parse_multipolygon(out);
            } else if (subtype == "GEOMETRYCOLLECTION") {
                success = parse_geometrycollection(out);  // Recursive
            } else {
                throw std::runtime_error("Unknown geometry type in GEOMETRYCOLLECTION: " + subtype);
            }
            
            if (!success) {
                return false;
            }
            
            skip_whitespace();
            if (peek() == ',') advance();
        }
        
        if (!match(')')) {
            throw std::runtime_error("Expected ')' at end of GEOMETRYCOLLECTION");
        }
        return true;
    }
};

WKTResult parse_wkt(const std::string& wkt) {
    WKTParser parser;
    return parser.parse(wkt);
}
