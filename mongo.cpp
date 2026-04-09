#include "mongo.hpp"
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#include "errors.hpp"
#include "error_logger.hpp"

std::unique_ptr<mongocxx::instance> MongoWriter::global_instance = nullptr;
std::atomic_flag MongoWriter::initialized = ATOMIC_FLAG_INIT;
std::once_flag MongoWriter::collection_drop_flag;
std::once_flag MongoWriter::index_create_flag;

thread_local static std::unique_ptr<MongoWriter> tls_mongo_writer;

static std::atomic<size_t> global_total_tiles{0};
static std::atomic<size_t> global_total_batches{0};
static std::atomic<size_t> global_total_retries{0};
static std::atomic<size_t> global_total_errors{0};

std::atomic<size_t> MongoWriter::pending_writes{0};

MongoWriter* MongoWriter::get_thread_local_instance(const mongo_config &cfg) {
    if (!tls_mongo_writer) {
        tls_mongo_writer = std::make_unique<MongoWriter>(cfg);
        tls_mongo_writer->initialize_thread();
    }
    return tls_mongo_writer.get();
}

size_t MongoWriter::get_global_total_tiles() {
    return global_total_tiles.load();
}

size_t MongoWriter::get_global_total_batches() {
    return global_total_batches.load();
}

size_t MongoWriter::get_global_total_retries() {
    return global_total_retries.load();
}

size_t MongoWriter::get_global_total_errors() {
    return global_total_errors.load();
}

void MongoWriter::destroy_current_thread_instance() {
    if (tls_mongo_writer) {
        global_total_tiles += tls_mongo_writer->getTotalTilesWritten();
        global_total_batches += tls_mongo_writer->getTotalBatchesWritten();
        global_total_retries += tls_mongo_writer->getTotalRetries();
        global_total_errors += tls_mongo_writer->getTotalErrors();

        tls_mongo_writer->close();
        tls_mongo_writer.reset();
    }
}

extern int quiet;

MongoWriter::MongoWriter(const mongo_config &cfg)
    : config(cfg), use_upsert(!cfg.drop_collection_before_write)
{
    if (config.dbname.empty()) {
        throw std::runtime_error("MongoDB database name is required");
    }

    if (config.batch_size < MIN_MONGO_BATCH_SIZE) {
        config.batch_size = MIN_MONGO_BATCH_SIZE;
    } else if (config.batch_size > MAX_MONGO_BATCH_SIZE) {
        config.batch_size = MAX_MONGO_BATCH_SIZE;
    }

    batch_buffer.reserve(config.batch_size);
    batch_coords.reserve(config.batch_size);
}

MongoWriter::~MongoWriter() noexcept
{
    close();
}

void MongoWriter::initialize_global()
{
    if (!initialized.test_and_set(std::memory_order_acquire)) {
        global_instance = std::make_unique<mongocxx::instance>();
    }
}

void MongoWriter::initialize_thread()
{
    try {
        mongocxx::uri uri(config.uri());
        client = std::make_unique<mongocxx::client>(uri);

        collection = (*client)[config.dbname][config.collection];

        if (config.drop_collection_before_write) {
            std::call_once(collection_drop_flag, [this]() {
                try {
                    collection.drop();
                    if (!quiet) {
                        fprintf(stderr, "MongoDB: dropped collection %s.%s\n",
                                config.dbname.c_str(), config.collection.c_str());
                    }
                } catch (const std::exception &e) {
                    fprintf(stderr, "Warning: failed to drop MongoDB collection %s.%s: %s\n",
                            config.dbname.c_str(), config.collection.c_str(), e.what());
                }
            });
        }

        if (config.create_indexes) {
            std::call_once(index_create_flag, [this]() {
                create_indexes_if_needed();
            });
        }

        if (config.enable_progress_report && !quiet) {
            fprintf(stderr, "MongoDB connected: %s.%s (pool: %zu, write concern: %d, mode: %s)\n",
                    config.dbname.c_str(), config.collection.c_str(), config.connection_pool_size,
                    static_cast<int>(config.write_concern_level),
                    use_upsert ? "upsert" : "insert");
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "Error: MongoDB connection initialization failed: %s\n", e.what());
        ErrorLogger::instance().log_error(ErrorSource::MONGO_CONNECT, 0, 0, 0,
                                          std::string("MongoDB init failed: ") + e.what());
        throw;
    }
}

void MongoWriter::write_tile(int z, int x, int y, const char *data, size_t len)
{
    while (pending_writes.load(std::memory_order_relaxed) >= MAX_PENDING_WRITES) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    pending_writes.fetch_add(1);

    auto doc = bsoncxx::builder::stream::document{}
        << "x" << x
        << "y" << y
        << "z" << z
        << "c" << 1
        << "d" << bsoncxx::types::b_binary{
            bsoncxx::binary_sub_type::k_binary,
            static_cast<uint32_t>(len),
            reinterpret_cast<const uint8_t*>(data)
        }
        << bsoncxx::builder::stream::finalize;

    batch_buffer.push_back(std::move(doc));
    batch_coords.push_back({z, x, y});

    if (batch_buffer.size() >= config.batch_size) {
        flush_batch();
    }
}

void MongoWriter::flush_all() noexcept
{
    try {
        if (!batch_buffer.empty()) {
            flush_batch();
        }
    } catch (const std::exception &e) {
        total_errors++;
        ErrorLogger::instance().log_error(ErrorSource::MONGO_FLUSH, 0, 0, 0,
                                          std::string("flush_all failed: ") + e.what());
    } catch (...) {
        total_errors++;
        ErrorLogger::instance().log_error(ErrorSource::MONGO_FLUSH, 0, 0, 0,
                                          "flush_all failed (unknown error)");
    }
}

void MongoWriter::close() noexcept
{
    try {
        flush_all();
    } catch (...) {
        total_errors++;
    }

    try {
        if (client) {
            client.reset();
        }
    } catch (...) {
        total_errors++;
    }
}

void MongoWriter::build_write_concern()
{
    if (wc_initialized) {
        return;
    }

    switch (config.write_concern_level) {
        case WriteConcernLevel::NONE:
            cached_wc.acknowledge_level(mongocxx::write_concern::level::k_unacknowledged);
            break;
        case WriteConcernLevel::PRIMARY:
            cached_wc.acknowledge_level(mongocxx::write_concern::level::k_acknowledged);
            cached_wc.nodes(1);
            break;
        case WriteConcernLevel::MAJORITY:
            cached_wc.acknowledge_level(mongocxx::write_concern::level::k_majority);
            break;
    }

    if (config.journal) {
        cached_wc.journal(true);
    }

    if (config.wtimeout_ms > 0) {
        cached_wc.timeout(std::chrono::milliseconds(config.wtimeout_ms));
    }

    wc_initialized = true;
}

void MongoWriter::flush_batch()
{
    if (batch_buffer.empty()) {
        return;
    }

    build_write_concern();

    int attempts = 0;

    while (attempts < config.max_retries) {
        try {
            if (use_upsert) {
                mongocxx::options::bulk_write bulk_opts;
                bulk_opts.ordered(false);
                bulk_opts.write_concern(cached_wc);
                auto bulk = collection.create_bulk_write(bulk_opts);

                for (size_t i = 0; i < batch_buffer.size(); i++) {
                    bsoncxx::document::view view = batch_buffer[i].view();
                    const auto &coord = batch_coords[i];

                    auto filter = bsoncxx::builder::stream::document{}
                        << "z" << coord.z
                        << "x" << coord.x
                        << "y" << coord.y
                        << bsoncxx::builder::stream::finalize;

                    mongocxx::model::replace_one upsert_op(filter.view(), view);
                    upsert_op.upsert(true);
                    bulk.append(upsert_op);
                }

                bulk.execute();
            } else {
                mongocxx::options::insert insert_opts;
                insert_opts.bypass_document_validation(false);
                insert_opts.ordered(false);
                insert_opts.write_concern(cached_wc);

                std::vector<bsoncxx::document::view> views;
                views.reserve(batch_buffer.size());
                for (const auto &doc : batch_buffer) {
                    views.push_back(doc.view());
                }

                collection.insert_many(views, insert_opts);
            }

            flush_failure_rounds = 0;
            total_tiles_written += batch_buffer.size();
            total_batches_written++;
            pending_writes.fetch_sub(batch_buffer.size());

            batch_buffer.clear();
            batch_coords.clear();
            return;

        } catch (const std::exception &e) {
            attempts++;
            total_errors++;

            if (!quiet) {
                fprintf(stderr, "MongoDB flush_batch_%s failed (attempt %d/%d): %s\n",
                        use_upsert ? "upsert" : "insert", attempts, config.max_retries, e.what());
            }

            for (size_t i = 0; i < batch_coords.size(); i++) {
                const auto &coord = batch_coords[i];
                ErrorLogger::instance().log_mongo_error(
                    coord.z, coord.x, coord.y,
                    use_upsert ? "flush_batch_upsert" : "flush_batch_insert",
                    std::string("attempt ") + std::to_string(attempts) + "/" + std::to_string(config.max_retries) + ": " + e.what());
            }

            if (attempts >= config.max_retries) {
                total_failed_batches++;
                flush_failure_rounds++;

                fprintf(stderr, "Error: MongoDB flush_batch_%s failed after %d attempts (round %zu). %zu tiles remain in buffer.\n",
                        use_upsert ? "upsert" : "insert", config.max_retries, flush_failure_rounds.load(), batch_buffer.size());

                if (flush_failure_rounds.load() >= 3) {
                    fprintf(stderr, "Warning: Persistent MongoDB failures (%zu rounds). Discarding %zu tiles to prevent stall.\n",
                            flush_failure_rounds.load(), batch_buffer.size());
                    pending_writes.fetch_sub(batch_buffer.size());
                    batch_buffer.clear();
                    batch_coords.clear();
                    flush_failure_rounds = 0;
                }

                throw;
            }

            try {
                reconnect();
                total_retries++;
            } catch (const std::exception &reconnect_e) {
                if (!quiet) {
                    fprintf(stderr, "MongoDB reconnect failed: %s\n", reconnect_e.what());
                }
            }
        }
    }
}

void MongoWriter::reconnect()
{
    int shift = std::min(consecutive_reconnects, static_cast<size_t>(5));
    int wait_ms = 100 * (1 << shift);
    wait_ms = std::min(wait_ms, 5000);
    consecutive_reconnects++;

    if (!quiet && wait_ms > 100) {
        fprintf(stderr, "MongoDB reconnecting in %d ms...\n", wait_ms);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));

    try {
        mongocxx::uri uri(config.uri());
        client = std::make_unique<mongocxx::client>(uri);
        collection = (*client)[config.dbname][config.collection];
        consecutive_reconnects = 0;

        if (!quiet) {
            fprintf(stderr, "MongoDB reconnected successfully\n");
        }

    } catch (const std::exception &e) {
        fprintf(stderr, "MongoDB reconnect failed: %s\n", e.what());
        throw;
    }
}

void MongoWriter::create_indexes_if_needed()
{
    try {
        auto index_view = collection.indexes();

        bool unique_index_exists = false;
        bool zoom_index_exists = false;

        try {
            for (const auto &index : index_view.list()) {
                auto name_element = index["name"];
                if (name_element &&
                    name_element.get_string().value == bsoncxx::string::view_or_value("tile_coords_unique")) {
                    unique_index_exists = true;
                }
                if (name_element &&
                    name_element.get_string().value == bsoncxx::string::view_or_value("zoom_level")) {
                    zoom_index_exists = true;
                }
            }
        } catch (const std::exception &e) {
            if (!quiet) {
                fprintf(stderr, "Warning: Could not list MongoDB indexes: %s. Will attempt to create.\n", e.what());
            }
        }

        if (!unique_index_exists) {
            try {
                auto keys = bsoncxx::builder::stream::document{}
                    << "z" << 1
                    << "x" << 1
                    << "y" << 1
                    << bsoncxx::builder::stream::finalize;

                auto options = bsoncxx::builder::stream::document{}
                    << "unique" << true
                    << "name" << "tile_coords_unique"
                    << bsoncxx::builder::stream::finalize;

                index_view.create_one(keys.view(), options.view());

                if (!quiet) {
                    fprintf(stderr, "Created unique index on (z, x, y) for %s.%s\n",
                            config.dbname.c_str(), config.collection.c_str());
                }
            } catch (const std::exception &e) {
                if (!quiet) {
                    fprintf(stderr, "Note: Unique index on (z, x, y) may already exist: %s\n", e.what());
                }
            }
        }

        if (!zoom_index_exists) {
            try {
                auto keys = bsoncxx::builder::stream::document{}
                    << "z" << 1
                    << bsoncxx::builder::stream::finalize;

                auto options = bsoncxx::builder::stream::document{}
                    << "name" << "zoom_level"
                    << bsoncxx::builder::stream::finalize;

                index_view.create_one(keys.view(), options.view());

                if (!quiet) {
                    fprintf(stderr, "Created index on z for %s.%s\n",
                            config.dbname.c_str(), config.collection.c_str());
                }
            } catch (const std::exception &e) {
                if (!quiet) {
                    fprintf(stderr, "Note: Index on z may already exist: %s\n", e.what());
                }
            }
        }

    } catch (const std::exception &e) {
        fprintf(stderr, "Warning: Failed to create MongoDB indexes: %s\n", e.what());
    }
}

void MongoWriter::erase_zoom(int z)
{
    try {
        auto filter = bsoncxx::builder::stream::document{}
            << "z" << z
            << bsoncxx::builder::stream::finalize;

        auto result = collection.delete_many(filter.view());

        if (!quiet) {
            fprintf(stderr, "MongoDB: deleted %d tiles at z=%d\n",
                    z, static_cast<int>(result->deleted_count()));
        }

        if (!use_upsert) {
            use_upsert = true;
            if (!quiet) {
                fprintf(stderr, "MongoDB: switched to upsert mode after erase_zoom(z=%d) to prevent duplicate key errors\n", z);
            }
        }
    } catch (const std::exception &e) {
        if (!quiet) {
            fprintf(stderr, "Warning: failed to delete MongoDB tiles at z=%d: %s\n",
                    z, e.what());
        }
    } catch (...) {
        if (!quiet) {
            fprintf(stderr, "Warning: failed to delete MongoDB tiles at z=%d (unknown error)\n", z);
        }
    }
}

void MongoWriter::write_metadata(const std::string &json_metadata)
{
    try {
        auto db = (*client)[config.dbname];
        std::string meta_collection_name = config.collection + "_metadata";
        auto meta_collection = db[meta_collection_name];

        meta_collection.drop();

        auto doc = bsoncxx::builder::stream::document{}
            << "metadata" << json_metadata
            << "collection" << config.collection
            << "timestamp" << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()
            << bsoncxx::builder::stream::finalize;

        meta_collection.insert_one(doc.view());

        if (!quiet) {
            fprintf(stderr, "MongoDB: wrote metadata to %s.%s\n",
                    config.dbname.c_str(), meta_collection_name.c_str());
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "Warning: failed to write MongoDB metadata: %s\n", e.what());
    }
}
