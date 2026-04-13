#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>
#include <cctype>

// PostgreSQL/PostGIS Configuration
constexpr size_t MAX_POSTGRES_CONNECTIONS = 50;
constexpr size_t DEFAULT_POSTGIS_BATCH_SIZE = 1000;
constexpr size_t MAX_POSTGIS_BATCH_SIZE = 10000;
constexpr size_t MIN_POSTGIS_BATCH_SIZE = 100;
constexpr size_t MAX_POSTGIS_MEMORY_USAGE_MB = 512;
constexpr int MAX_POSTGIS_RETRIES = 3;
constexpr int POSTGIS_CONNECTION_TIMEOUT_SEC = 30;

// MongoDB Configuration
constexpr size_t DEFAULT_MONGO_BATCH_SIZE = 100;
constexpr size_t MAX_MONGO_BATCH_SIZE = 1000;
constexpr size_t MIN_MONGO_BATCH_SIZE = 10;
constexpr size_t MAX_MONGO_CONNECTION_POOL_SIZE = 50;
constexpr size_t DEFAULT_MONGO_CONNECTION_POOL_SIZE = 10;
constexpr int MONGO_TIMEOUT_MS = 30000;
constexpr int MONGO_MAX_RETRIES = 3;

// MongoDB Batch Size Thresholds
constexpr size_t MONGO_BATCH_TIER_1 = 10000;
constexpr size_t MONGO_BATCH_TIER_2 = 100000;
constexpr size_t MONGO_BATCH_TIER_3 = 500000;
constexpr size_t MONGO_BATCH_TIER_4 = 2000000;
constexpr size_t MONGO_BATCH_SIZE_TIER_1 = 100;
constexpr size_t MONGO_BATCH_SIZE_TIER_2 = 200;
constexpr size_t MONGO_BATCH_SIZE_TIER_3 = 500;
constexpr size_t MONGO_BATCH_SIZE_TIER_4 = 800;
constexpr size_t MONGO_BATCH_SIZE_TIER_5 = 1000;

inline std::vector<std::string> split_by_delimiter(const std::string &str, char delim) {
    std::vector<std::string> parts;
    std::string current;
    for (size_t i = 0; i < str.size(); i++) {
        if (str[i] == delim) {
            parts.push_back(current);
            current.clear();
        } else {
            current += str[i];
        }
    }
    parts.push_back(current);
    return parts;
}

inline std::vector<std::string> split_csv_list(const std::string &csv) {
    std::vector<std::string> parts = split_by_delimiter(csv, ',');
    std::vector<std::string> out;
    out.reserve(parts.size());
    for (size_t i = 0; i < parts.size(); i++) {
        const std::string &s = parts[i];
        size_t start = 0;
        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
            start++;
        }
        size_t end = s.size();
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
            end--;
        }
        if (end > start) {
            out.push_back(s.substr(start, end - start));
        }
    }
    return out;
}

// Debug Configuration
#ifdef DEBUG
    #define DEBUG_LOG(fmt, ...) fprintf(stderr, "Debug: " fmt "\n", ##__VA_ARGS__)
#else
    #define DEBUG_LOG(fmt, ...) ((void)0)
#endif

#endif
