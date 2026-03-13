#ifndef POSTGIS_HPP
#define POSTGIS_HPP

#include <string>
#include <vector>
#include <atomic>
#include "geometry.hpp"
#include "serial.hpp"

// Forward declaration for libpq types
typedef struct pg_result PGresult;

// Performance optimization constants
constexpr size_t DEFAULT_BATCH_SIZE = 1000;        // Number of features to process per batch
constexpr size_t MAX_BATCH_SIZE = 10000;           // Maximum batch size
constexpr size_t MIN_BATCH_SIZE = 100;             // Minimum batch size
constexpr size_t MAX_MEMORY_USAGE_MB = 512;        // Maximum memory usage in MB
constexpr int MAX_RETRIES = 3;                     // Maximum retry attempts for database operations
constexpr int CONNECTION_TIMEOUT_SEC = 30;         // Connection timeout in seconds

struct postgis_config
{
    std::string host;
    std::string port;
    std::string dbname;
    std::string user;
    std::string password;
    std::string table;
    std::string geometry_field;
    std::string sql;
    
    // Performance optimization settings
    size_t batch_size;              // Number of features per batch
    bool use_cursor;                // Use cursor for large datasets
    size_t max_memory_mb;           // Maximum memory usage in MB
    int max_retries;                // Maximum retry attempts
    bool enable_progress_report;    // Enable progress reporting

    postgis_config()
        : host("localhost"),
          port("5432"),
          dbname(""),
          user(""),
          password(""),
          table(""),
          geometry_field("geometry"),
          sql(""),
          batch_size(DEFAULT_BATCH_SIZE),
          use_cursor(true),
          max_memory_mb(MAX_MEMORY_USAGE_MB),
          max_retries(MAX_RETRIES),
          enable_progress_report(true)
    {
    }
};

class PostGISReader
{
public:
    PostGISReader(const postgis_config &cfg);
    ~PostGISReader();

    bool connect();
    bool read_features(std::vector<struct serialization_state> &sst, size_t layer, const std::string &layername);
    
    // Get statistics
    size_t getTotalFeaturesProcessed() const { return total_features_processed.load(); }
    size_t getTotalBatchesProcessed() const { return total_batches_processed.load(); }
    size_t getCurrentMemoryUsage() const { return current_memory_usage.load(); }

protected:
    bool execute_query(const std::string &query);
    bool execute_query_with_retry(const std::string &query);
    void process_batch(PGresult *res, std::vector<struct serialization_state> &sst, 
                      size_t layer, const std::string &layername, int geom_field_index);
    void process_feature(PGresult *res, int row, int nfields, int geom_field_index,
                        std::vector<struct serialization_state> &sst, size_t layer, 
                        const std::string &layername);
    std::string escape_json_string(const char *value);
    bool check_memory_usage();
    void log_progress(size_t processed, size_t total, const char *stage);

private:
    postgis_config config;
    void *conn;
    key_pool kpool;
    
    // Statistics and monitoring
    std::atomic<size_t> total_features_processed{0};
    std::atomic<size_t> total_batches_processed{0};
    std::atomic<size_t> current_memory_usage{0};
    std::atomic<size_t> peak_memory_usage{0};
    
    // Buffer reuse for performance
    std::string feature_buffer;
    std::string properties_buffer;
};

#endif // POSTGIS_HPP