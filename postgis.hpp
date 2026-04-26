#ifndef POSTGIS_HPP
#define POSTGIS_HPP

#include <string>
#include <vector>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <cctype>
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
    std::string shard_key;
    std::string shard_mode;
    std::string selected_columns_csv;
    bool selected_columns_best_effort;
    bool progress_with_exact_count;
    bool canonical_attr_order;
    bool profile;

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
          shard_key(""),
          shard_mode("auto"),
          selected_columns_csv(""),
          selected_columns_best_effort(false),
          progress_with_exact_count(false),
          canonical_attr_order(false),
          profile(false),
          batch_size(DEFAULT_POSTGIS_BATCH_SIZE),
          use_cursor(true),
          max_memory_mb(MAX_POSTGIS_MEMORY_USAGE_MB),
          max_retries(MAX_POSTGIS_RETRIES),
          enable_progress_report(true)
    {
    }

    bool has_sql_input() const {
        return !sql.empty();
    }

    bool has_table_input() const {
        return !table.empty();
    }

    bool has_input_source() const {
        return has_sql_input() || has_table_input();
    }

    bool sql_takes_precedence() const {
        return has_sql_input();
    }

    bool geometry_field_uses_default() const {
        return geometry_field == "geometry";
    }

    bool uses_default_shard_mode() const {
        return shard_mode == "auto";
    }

    static bool is_supported_shard_mode(const std::string &mode) {
        return mode == "auto" || mode == "none" || mode == "key" || mode == "range";
    }

    bool has_supported_shard_mode() const {
        return is_supported_shard_mode(effective_shard_mode());
    }

    bool is_auto_shard_mode() const {
        return effective_shard_mode() == "auto";
    }

    bool is_none_shard_mode() const {
        return effective_shard_mode() == "none";
    }

    bool is_key_shard_mode() const {
        return effective_shard_mode() == "key";
    }

    bool is_range_shard_mode() const {
        return effective_shard_mode() == "range";
    }

    bool has_selected_columns() const {
        return !selected_columns_csv.empty();
    }

    std::string effective_shard_mode() const {
        return shard_mode.empty() ? "auto" : shard_mode;
    }

    bool should_report_geometry_field() const {
        return !geometry_field.empty() && !geometry_field_uses_default();
    }

    bool should_report_selected_columns() const {
        return has_selected_columns();
    }

    bool should_report_shard_strategy() const {
        return !uses_default_shard_mode() || !shard_key.empty();
    }

    bool requires_selected_columns_for_best_effort() const {
        return selected_columns_best_effort && selected_columns_csv.empty();
    }

    bool requires_shard_key() const {
        return (is_key_shard_mode() || is_range_shard_mode()) && shard_key.empty();
    }

    bool ignores_shard_key() const {
        return !shard_key.empty() && is_none_shard_mode();
    }

    bool can_attempt_range_sharding() const {
        return is_range_shard_mode() || is_auto_shard_mode();
    }

    bool can_attempt_key_sharding() const {
        return is_key_shard_mode() || is_auto_shard_mode();
    }

    bool can_attempt_ctid_sharding() const {
        return is_auto_shard_mode();
    }

    bool has_read_tuning_overrides() const {
        return batch_size != DEFAULT_POSTGIS_BATCH_SIZE ||
               !use_cursor ||
               max_memory_mb != MAX_POSTGIS_MEMORY_USAGE_MB ||
               max_retries != MAX_POSTGIS_RETRIES ||
               !enable_progress_report;
    }

    bool uses_default_batch_size() const {
        return batch_size == DEFAULT_POSTGIS_BATCH_SIZE;
    }

    bool uses_cursor_by_default() const {
        return use_cursor;
    }

    bool uses_default_memory_limit() const {
        return max_memory_mb == MAX_POSTGIS_MEMORY_USAGE_MB;
    }

    bool uses_default_retry_policy() const {
        return max_retries == MAX_POSTGIS_RETRIES;
    }

    bool uses_default_progress_report() const {
        return enable_progress_report;
    }

    bool has_attribute_strategy_overrides() const {
        return selected_columns_best_effort || canonical_attr_order;
    }

    bool uses_default_attribute_order() const {
        return !canonical_attr_order;
    }

    bool has_debug_strategy_overrides() const {
        return progress_with_exact_count || profile;
    }

    bool uses_default_exact_count_strategy() const {
        return !progress_with_exact_count;
    }

    bool uses_default_profile_strategy() const {
        return !profile;
    }

    bool requires_explicit_geometry_field() const {
        return has_table_input() && !has_sql_input() && geometry_field.empty();
    }

    std::string input_mode_label() const {
        return has_sql_input() ? "sql" : "table";
    }

    std::string effective_layer_name() const {
        return has_table_input() ? table : "postgis";
    }

    void normalize() {
        if (shard_mode.empty()) {
            shard_mode = "auto";
        }

        if (batch_size < MIN_POSTGIS_BATCH_SIZE) {
            batch_size = MIN_POSTGIS_BATCH_SIZE;
        } else if (batch_size > MAX_POSTGIS_BATCH_SIZE) {
            batch_size = MAX_POSTGIS_BATCH_SIZE;
        }

        if (max_retries < 1) {
            max_retries = 1;
        }
    }

    bool parse_connection_string(const std::string &conn_str) {
        std::vector<std::string> parts = split_by_delimiter(conn_str, ':');

        if (parts.empty() || parts[0].empty()) {
            fprintf(stderr, "Error: PostGIS connection string is empty\n");
            return false;
        }

        bool legacy_format = false;
        if (parts.size() >= 3 && !parts[1].empty()) {
            legacy_format = true;
            for (char c : parts[1]) {
                if (!std::isdigit(static_cast<unsigned char>(c))) {
                    legacy_format = false;
                    break;
                }
            }
        }

        if (legacy_format) {
            host = parts[0];
            port = parts[1];
            dbname = parts[2];
            if (parts.size() >= 4) user = parts[3];
            if (parts.size() >= 5) password = parts[4];
            if (parts.size() >= 6) table = parts[5];
            if (parts.size() >= 7) geometry_field = parts[6];
            if (parts.size() >= 8) sql = parts[7];
        } else {
            if (parts.size() > 5) {
                fprintf(stderr, "Error: PostGIS short connection string supports at most 5 parts\n");
                fprintf(stderr, "Short format: dbname[:user[:password[:host[:port]]]]\n");
                fprintf(stderr, "Legacy format: host:port:dbname[:user[:password[:table[:geometry_field[:sql]]]]]\n");
                fprintf(stderr, "Got %zu parts: %s\n", parts.size(), conn_str.c_str());
                return false;
            }

            dbname = parts[0];
            if (parts.size() >= 2) user = parts[1];
            if (parts.size() >= 3) password = parts[2];
            if (parts.size() >= 4 && !parts[3].empty()) host = parts[3];
            if (parts.size() >= 5 && !parts[4].empty()) port = parts[4];
        }

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
    static std::string build_select_query(const postgis_config &cfg, int srid, void *conn);
    static std::string build_sharded_query(const std::string &base_query, const std::string &shard_condition);
    static void reset_runtime_caches();

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

    static std::unordered_map<std::string, int> srid_cache_;
    static std::mutex srid_cache_mutex_;
};

#endif
