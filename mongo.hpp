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

    bool enable_progress_report;

    // If true, any discarded tiles cause a non-zero process exit code.
    bool fail_on_discard;

    mongo_config()
        : host(""),
          port(0),
          dbname(""),
          collection(""),
          username(""),
          password(""),
          auth_source(""),
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
    }

    bool parse_connection_string(const std::string &conn_str) {
        std::vector<std::string> parts = split_by_delimiter(conn_str, ':');

        if (parts.size() != 7) {
            fprintf(stderr, "Error: MongoDB connection string must have exactly 7 parts\n");
            fprintf(stderr, "Format: host:port:dbname:user:password:auth_source:collection\n");
            fprintf(stderr, "Example: localhost:27017:gis:user:pass:admin:china\n");
            fprintf(stderr, "Got %zu parts: %s\n", parts.size(), conn_str.c_str());
            return false;
        }

        host = parts[0];
        if (!parts[1].empty()) {
            try {
                port = std::stoi(parts[1]);
            } catch (...) {
                fprintf(stderr, "Error: Invalid port number: %s\n", parts[1].c_str());
                return false;
            }
        } else {
            port = 27017;
        }
        dbname = parts[2];
        username = parts[3];
        password = parts[4];
        auth_source = parts[5];
        collection = parts[6];

        if (dbname.empty() || username.empty() || auth_source.empty() || collection.empty()) {
            fprintf(stderr, "Error: MongoDB connection string has empty required fields\n");
            fprintf(stderr, "Format: host:port:dbname:user:password:auth_source:collection\n");
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
};

class MongoWriter {
public:
    explicit MongoWriter(const mongo_config &cfg);

    ~MongoWriter() noexcept;

    static void initialize_global();

    static MongoWriter* get_thread_local_instance(const mongo_config &cfg);

    static MongoWriter* get_writer_instance(const mongo_config &cfg);

    static void destroy_current_thread_instance();

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

    void flush_batch();
    void flush_batch_with_retry(bool upsert_mode,
        std::vector<bsoncxx::document::value> batch_buf,
        std::vector<tile_coords> batch_coord);
    void create_indexes_if_needed(mongocxx::collection &collection);
    void build_write_concern();
    std::set<int> get_erased_zooms_snapshot() const;

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

    static std::unique_ptr<mongocxx::instance> global_instance;
    static std::atomic_flag initialized;
    static std::once_flag collection_drop_flag;
    static std::once_flag index_create_flag;
};

#endif // MONGO_HPP
