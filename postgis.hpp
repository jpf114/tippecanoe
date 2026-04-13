#ifndef POSTGIS_HPP
#define POSTGIS_HPP

#include <string>
#include <vector>
#include <atomic>
#include "geometry.hpp"
#include "serial.hpp"
#include "config.hpp"

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

    size_t batch_size;
    bool use_cursor;
    size_t max_memory_mb;
    int max_retries;
    bool enable_progress_report;

    postgis_config()
        : host("localhost"),
          port("5432"),
          dbname(""),
          user(""),
          password(""),
          table(""),
          geometry_field("geometry"),
          sql(""),
          batch_size(DEFAULT_POSTGIS_BATCH_SIZE),
          use_cursor(true),
          max_memory_mb(MAX_POSTGIS_MEMORY_USAGE_MB),
          max_retries(MAX_POSTGIS_RETRIES),
          enable_progress_report(true)
    {
    }

    bool parse_connection_string(const std::string &conn_str) {
        std::vector<std::string> parts = split_by_delimiter(conn_str, ':');

        if (parts.size() < 3) {
            fprintf(stderr, "Error: PostGIS connection string must have at least 3 parts\n");
            fprintf(stderr, "Format: host:port:dbname[:user[:password[:table[:geometry_field[:sql]]]]]\n");
            fprintf(stderr, "Example: localhost:5432:gis:user:pass:my_table:geom\n");
            fprintf(stderr, "Got %zu parts: %s\n", parts.size(), conn_str.c_str());
            return false;
        }

        host = parts[0];
        port = parts[1];
        dbname = parts[2];
        if (parts.size() >= 4) user = parts[3];
        if (parts.size() >= 5) password = parts[4];
        if (parts.size() >= 6) table = parts[5];
        if (parts.size() >= 7) geometry_field = parts[6];
        if (parts.size() >= 8) sql = parts[7];

        if (dbname.empty()) {
            fprintf(stderr, "Error: PostGIS connection string has empty dbname\n");
            return false;
        }

        return true;
    }
};

class PostGISReader
{
public:
    PostGISReader(const postgis_config &cfg);
    ~PostGISReader();

    bool connect();
    void disconnect();
    bool read_features(std::vector<struct serialization_state> &sst, size_t layer, const std::string &layername,
                       size_t thread_id = 0, size_t num_threads = 1);

    static int get_cached_srid(const postgis_config &cfg, void *conn);
    static std::string build_select_query(const postgis_config &cfg, int srid);

    size_t getTotalFeaturesProcessed() const { return total_features_processed.load(); }
    size_t getTotalBatchesProcessed() const { return total_batches_processed.load(); }
    size_t getCurrentMemoryUsage() const { return current_memory_usage.load(); }
    size_t getParseErrors() const { return parse_errors_.load(); }

protected:
    bool execute_query(const std::string &query);
    bool execute_query_with_retry(const std::string &query);
    void process_batch(PGresult *res, std::vector<struct serialization_state> &sst,
                      size_t layer, const std::string &layername, int wkb_field_index, size_t thread_id);
    void process_feature(PGresult *res, int row, int nfields, int wkb_field_index,
                        const std::vector<std::string> &field_names,
                        std::vector<struct serialization_state> &sst, size_t layer,
                        const std::string &layername, size_t thread_id);
    std::string escape_json_string(const char *value);
    bool check_memory_usage();
    void log_progress(size_t processed, size_t total, const char *stage);

    std::vector<uint8_t> decode_bytea(const char *hex_data, size_t hex_len);

private:
    postgis_config config;
    void *conn;

    std::atomic<size_t> total_features_processed{0};
    std::atomic<size_t> total_batches_processed{0};
    std::atomic<size_t> current_memory_usage{0};
    std::atomic<size_t> parse_errors_{0};

    static int cached_srid_;
    static bool srid_cached_;
};

#endif
