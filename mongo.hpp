#ifndef MONGO_HPP
#define MONGO_HPP

#include <string>
#include <vector>
#include <tuple>
#include <atomic>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <cctype>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/document/view.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>
#include <mongocxx/options/insert.hpp>
#include <mongocxx/options/replace.hpp>
#include <mongocxx/write_concern.hpp>
#include <mongocxx/options/client.hpp>
#include "config.hpp"
#include "mbtiles.hpp"

enum class WriteConcernLevel {
    NONE = 0,
    PRIMARY = 1,
    MAJORITY = 2
};

struct mongo_config {
    std::string host;
    int port;
    std::string dbname;
    std::string collection;
    std::string username;
    std::string password;
    std::string auth_source;

    size_t batch_size;
    size_t connection_pool_size;
    int timeout_ms;
    int max_retries;

    WriteConcernLevel write_concern_level;
    bool journal;
    int wtimeout_ms;

    bool create_indexes;

    bool drop_collection_before_write;

    bool write_metadata;
    bool metadata_explicitly_set;
    bool batch_size_explicitly_set;

    bool enable_progress_report;

    // If true, any discarded tiles cause a non-zero process exit code.
    bool fail_on_discard;

    mongo_config()
        : host("localhost"),
          port(27017),
          dbname(""),
          collection(""),
          username(""),
          password(""),
          auth_source("admin"),
          batch_size(DEFAULT_MONGO_BATCH_SIZE),
          connection_pool_size(DEFAULT_MONGO_CONNECTION_POOL_SIZE),
          timeout_ms(MONGO_TIMEOUT_MS),
          max_retries(MONGO_MAX_RETRIES),
          write_concern_level(WriteConcernLevel::PRIMARY),
          journal(false),
          wtimeout_ms(5000),
          create_indexes(true),
          drop_collection_before_write(false),
          write_metadata(false),
          metadata_explicitly_set(false),
          batch_size_explicitly_set(false),
          enable_progress_report(true),
          fail_on_discard(true)
    {
    }

    void normalize() {
        if (batch_size < MIN_MONGO_BATCH_SIZE) {
            batch_size = MIN_MONGO_BATCH_SIZE;
        } else if (batch_size > MAX_MONGO_BATCH_SIZE) {
            batch_size = MAX_MONGO_BATCH_SIZE;
        }

        if (connection_pool_size < 1) {
            connection_pool_size = 1;
        } else if (connection_pool_size > MAX_MONGO_CONNECTION_POOL_SIZE) {
            connection_pool_size = MAX_MONGO_CONNECTION_POOL_SIZE;
        }

        if (timeout_ms < 1) {
            timeout_ms = MONGO_TIMEOUT_MS;
        }

        if (max_retries < 1) {
            max_retries = 1;
        }
    }

    static std::string url_decode(const std::string &s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '%' && i + 2 < s.size() &&
                std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
                char hex[3] = {s[i + 1], s[i + 2], '\0'};
                out += static_cast<char>(strtol(hex, nullptr, 16));
                i += 2;
            } else if (s[i] == '+') {
                out += ' ';
            } else {
                out += s[i];
            }
        }
        return out;
    }

    bool parse_connection_string(const std::string &conn_str) {
        if (conn_str.empty()) {
            fprintf(stderr, "Error: MongoDB connection string is empty\n");
            return false;
        }

        std::string str = conn_str;

        std::string scheme;
        auto scheme_end = str.find("://");
        if (scheme_end != std::string::npos) {
            scheme = str.substr(0, scheme_end);
            for (auto &c : scheme) c = std::tolower(static_cast<unsigned char>(c));
            if (scheme != "mongodb" && scheme != "mongodb+srv") {
                fprintf(stderr, "Error: MongoDB connection string must start with mongodb:// or mongodb+srv://\n");
                fprintf(stderr, "Format: mongodb://[user:password@]host[:port]/dbname?collection=name[&authSource=src]\n");
                return false;
            }
            str = str.substr(scheme_end + 3);
        } else {
            fprintf(stderr, "Error: MongoDB connection string must start with mongodb://\n");
            fprintf(stderr, "Format: mongodb://[user:password@]host[:port]/dbname?collection=name[&authSource=src]\n");
            return false;
        }

        std::string userinfo;
        auto at_pos = str.rfind('@');
        if (at_pos != std::string::npos) {
            userinfo = str.substr(0, at_pos);
            str = str.substr(at_pos + 1);
        }

        if (!userinfo.empty()) {
            auto colon_pos = userinfo.find(':');
            if (colon_pos != std::string::npos) {
                username = url_decode(userinfo.substr(0, colon_pos));
                password = url_decode(userinfo.substr(colon_pos + 1));
            } else {
                username = url_decode(userinfo);
            }
        }

        std::string hostport;
        std::string path_and_query;
        auto slash_pos = str.find('/');
        if (slash_pos != std::string::npos) {
            hostport = str.substr(0, slash_pos);
            path_and_query = str.substr(slash_pos + 1);
        } else {
            hostport = str;
        }

        if (hostport.empty()) {
            fprintf(stderr, "Error: MongoDB connection string missing host\n");
            fprintf(stderr, "Format: mongodb://[user:password@]host[:port]/dbname?collection=name[&authSource=src]\n");
            return false;
        }

        auto bracket_close = hostport.find(']');
        if (bracket_close != std::string::npos) {
            host = hostport.substr(1, bracket_close - 1);
            if (bracket_close + 1 < hostport.size() && hostport[bracket_close + 1] == ':') {
                try {
                    port = std::stoi(hostport.substr(bracket_close + 2));
                } catch (...) {
                    fprintf(stderr, "Error: Invalid port number in MongoDB URI\n");
                    return false;
                }
            }
        } else {
            auto last_colon = hostport.rfind(':');
            if (last_colon != std::string::npos) {
                host = hostport.substr(0, last_colon);
                try {
                    port = std::stoi(hostport.substr(last_colon + 1));
                } catch (...) {
                    fprintf(stderr, "Error: Invalid port number: %s\n", hostport.substr(last_colon + 1).c_str());
                    return false;
                }
            } else {
                host = hostport;
            }
        }

        std::string query;
        auto query_pos = path_and_query.find('?');
        if (query_pos != std::string::npos) {
            dbname = url_decode(path_and_query.substr(0, query_pos));
            query = path_and_query.substr(query_pos + 1);
        } else {
            dbname = url_decode(path_and_query);
        }

        if (!query.empty()) {
            std::vector<std::string> params = split_by_delimiter(query, '&');
            for (const auto &param : params) {
                auto eq_pos = param.find('=');
                if (eq_pos == std::string::npos) continue;
                std::string key = param.substr(0, eq_pos);
                std::string val = url_decode(param.substr(eq_pos + 1));
                if (key == "collection") {
                    collection = val;
                } else if (key == "authSource") {
                    auth_source = val;
                }
            }
        }

        if (dbname.empty()) {
            fprintf(stderr, "Error: MongoDB connection string missing database name\n");
            fprintf(stderr, "Format: mongodb://[user:password@]host[:port]/dbname?collection=name[&authSource=src]\n");
            return false;
        }

        if (collection.empty()) {
            fprintf(stderr, "Error: MongoDB connection string missing collection (use ?collection=name)\n");
            fprintf(stderr, "Format: mongodb://[user:password@]host[:port]/dbname?collection=name[&authSource=src]\n");
            return false;
        }

        if (username.empty() != password.empty()) {
            fprintf(stderr, "Error: MongoDB username and password must be provided together\n");
            return false;
        }

        return true;
    }

    std::string uri() const {
        std::string uri_str = "mongodb://";

        if (!username.empty() && !password.empty()) {
            uri_str += username + ":" + password + "@";
        }

        uri_str += host + ":" + std::to_string(port) + "/";

        if (!dbname.empty()) {
            uri_str += dbname;
        }

        std::vector<std::string> opts;

        if (!auth_source.empty()) {
            opts.push_back("authSource=" + auth_source);
        }

        size_t actual_pool_size = std::min(connection_pool_size, MAX_MONGO_CONNECTION_POOL_SIZE);
        opts.push_back("maxPoolSize=" + std::to_string(actual_pool_size));
        opts.push_back("minPoolSize=1");

        opts.push_back("serverSelectionTimeoutMS=" + std::to_string(timeout_ms));
        opts.push_back("connectTimeoutMS=" + std::to_string(timeout_ms));
        opts.push_back("socketTimeoutMS=" + std::to_string(timeout_ms));

        switch (write_concern_level) {
            case WriteConcernLevel::NONE:
                opts.push_back("w=0");
                break;
            case WriteConcernLevel::PRIMARY:
                opts.push_back("w=1");
                break;
            case WriteConcernLevel::MAJORITY:
                opts.push_back("w=majority");
                break;
        }

        if (journal) {
            opts.push_back("journal=true");
        }

        if (wtimeout_ms > 0) {
            opts.push_back("wtimeoutMS=" + std::to_string(wtimeout_ms));
        }

        opts.push_back("retryReads=true");
        opts.push_back("retryWrites=true");

        if (!opts.empty()) {
            uri_str += "?";
            for (size_t i = 0; i < opts.size(); i++) {
                if (i > 0) uri_str += "&";
                uri_str += opts[i];
            }
        }

        return uri_str;
    }

    std::string safe_uri() const {
        std::string uri_str = "mongodb://";

        if (!username.empty()) {
            uri_str += username + ":****@";
        }

        uri_str += host + ":" + std::to_string(port) + "/" + dbname;
        return uri_str;
    }

    bool uses_default_pool_size() const {
        return connection_pool_size == DEFAULT_MONGO_CONNECTION_POOL_SIZE;
    }

    bool uses_default_timeout() const {
        return timeout_ms == MONGO_TIMEOUT_MS;
    }

    bool uses_default_indexes() const {
        return create_indexes;
    }

    bool uses_default_fail_policy() const {
        return fail_on_discard;
    }
};

class MongoWriter {
public:
    explicit MongoWriter(const mongo_config &cfg);

    ~MongoWriter() noexcept;

    static void initialize_global();

    static MongoWriter* get_thread_local_instance(const mongo_config &cfg);

    static MongoWriter* get_writer_instance(const mongo_config &cfg);

    static void destroy_current_thread_instance();
    static bool flush_current_thread_instance();

    static void destroy_global_instance();

    static void destroy_writer_instance();

    static size_t get_global_total_tiles();
    static size_t get_global_total_batches();
    static size_t get_global_total_retries();
    static size_t get_global_total_errors();
    static size_t get_global_total_discarded();
    static size_t get_global_pool_unavailable_batches();
    static size_t get_global_retry_exhausted_batches();
    static size_t get_global_insert_batches();
    static size_t get_global_upsert_batches();
    static size_t get_global_insert_discarded_tiles();
    static size_t get_global_upsert_discarded_tiles();

    void initialize_thread();

    void write_tile(int z, int x, int y, const char *data, size_t len);

    void flush_all() noexcept;

    size_t getTotalTilesWritten() const { return total_tiles_written.load(); }
    size_t getTotalBatchesWritten() const { return total_batches_written.load(); }
    size_t getCurrentBufferSize() const { return batch_buffer.size(); }
    size_t getTotalRetries() const { return total_retries.load(); }
    size_t getTotalErrors() const { return total_errors.load(); }
    size_t getPoolUnavailableBatches() const { return total_pool_unavailable_batches.load(); }
    size_t getRetryExhaustedBatches() const { return total_retry_exhausted_batches.load(); }
    size_t getInsertBatches() const { return total_insert_batches.load(); }
    size_t getUpsertBatches() const { return total_upsert_batches.load(); }
    size_t getTotalDiscardedTiles() const { return total_discarded_tiles.load(); }
    size_t getInsertDiscardedTiles() const { return total_insert_discarded_tiles.load(); }
    size_t getUpsertDiscardedTiles() const { return total_upsert_discarded_tiles.load(); }

    void close() noexcept;

    void erase_zoom(int z);

    void write_metadata_bson(const struct metadata &meta);

    bool should_use_upsert_for_zoom(int z) const;

    struct tile_coords {
        int z, x, y;
    };

private:
    static mongocxx::pool* get_or_create_pool(const mongo_config &cfg);
    static void reset_global_runtime_state();

    void flush_batch();
    void flush_batch_with_retry(bool upsert_mode,
        std::vector<bsoncxx::document::value> batch_buf,
        std::vector<tile_coords> batch_coord);
    void create_indexes_if_needed(mongocxx::collection &collection);
    void build_write_concern();
    std::set<int> get_erased_zooms_snapshot() const;
    void merge_stats_once();

    mongo_config config;

    std::vector<bsoncxx::document::value> batch_buffer;
    std::vector<tile_coords> batch_coords;

    mongocxx::write_concern cached_wc;
    bool wc_initialized{false};

    static std::mutex erased_zooms_mutex;
    static std::set<int> erased_zooms;

    std::atomic<size_t> total_tiles_written{0};
    std::atomic<size_t> total_batches_written{0};
    std::atomic<size_t> total_retries{0};
    std::atomic<size_t> total_errors{0};
    std::atomic<size_t> total_failed_batches{0};
    std::atomic<size_t> total_discarded_tiles{0};
    std::atomic<size_t> flush_failure_rounds{0};
    std::atomic<size_t> total_pool_unavailable_batches{0};
    std::atomic<size_t> total_retry_exhausted_batches{0};
    std::atomic<size_t> total_insert_batches{0};
    std::atomic<size_t> total_upsert_batches{0};
    std::atomic<size_t> total_insert_discarded_tiles{0};
    std::atomic<size_t> total_upsert_discarded_tiles{0};
    bool stats_merged_{false};

    static std::unique_ptr<mongocxx::instance> global_instance;
    static std::atomic_flag initialized;
    static std::once_flag collection_drop_flag;
    static std::once_flag index_create_flag;
};

#endif // MONGO_HPP
