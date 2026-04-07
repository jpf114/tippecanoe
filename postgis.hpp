#ifndef POSTGIS_HPP
#define POSTGIS_HPP

#include <string>
#include <vector>
#include <atomic>
#include "geometry.hpp"
#include "serial.hpp"
#include "config.hpp"

// Forward declaration for libpq types
typedef struct pg_result PGresult;

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
    std::string pk_field;
    
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
          pk_field(""),
          batch_size(DEFAULT_POSTGIS_BATCH_SIZE),
          use_cursor(true),
          max_memory_mb(MAX_POSTGIS_MEMORY_USAGE_MB),
          max_retries(MAX_POSTGIS_RETRIES),
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
    bool get_pk_range(long long &min_val, long long &max_val);
    bool read_features(std::vector<struct serialization_state> &sst, size_t layer, const std::string &layername,
                       long long min_pk = 0, long long max_pk = 0, bool has_range = false, size_t thread_id = 0);
    
    // Get statistics
    size_t getTotalFeaturesProcessed() const { return total_features_processed.load(); }
    size_t getTotalBatchesProcessed() const { return total_batches_processed.load(); }
    size_t getCurrentMemoryUsage() const { return current_memory_usage.load(); }

protected:
    bool execute_query(const std::string &query);
    bool execute_query_with_retry(const std::string &query);
    void process_batch(PGresult *res, std::vector<struct serialization_state> &sst, 
                      size_t layer, const std::string &layername, int geom_field_index, size_t thread_id);
    void process_feature(PGresult *res, int row, int nfields, int geom_field_index,
                        const std::vector<std::string> &field_names,
                        std::vector<struct serialization_state> &sst, size_t layer, 
                        const std::string &layername, size_t thread_id);
    std::string escape_json_string(const char *value);
    bool check_memory_usage();
    void log_progress(size_t processed, size_t total, const char *stage);

private:
    postgis_config config;
    void *conn;
    
    // Statistics and monitoring
    std::atomic<size_t> total_features_processed{0};
    std::atomic<size_t> total_batches_processed{0};
    std::atomic<size_t> current_memory_usage{0};
};

#endif // POSTGIS_HPP