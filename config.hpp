#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <cstddef>
#include <cstdio>

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

// Debug Configuration
#ifdef DEBUG
    #define DEBUG_LOG(fmt, ...) fprintf(stderr, "Debug: " fmt "\n", ##__VA_ARGS__)
#else
    #define DEBUG_LOG(fmt, ...) ((void)0)
#endif

#endif
