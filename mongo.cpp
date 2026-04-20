#include "mongo.hpp"
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#include <random>
#include "errors.hpp"
#include "error_logger.hpp"

std::unique_ptr<mongocxx::instance> MongoWriter::global_instance = nullptr;
std::atomic_flag MongoWriter::initialized = ATOMIC_FLAG_INIT;
std::mutex MongoWriter::erased_zooms_mutex;
std::set<int> MongoWriter::erased_zooms;

thread_local static std::unique_ptr<MongoWriter> tls_mongo_writer;

static std::atomic<size_t> global_total_tiles{0};
static std::atomic<size_t> global_total_batches{0};
static std::atomic<size_t> global_total_retries{0};
static std::atomic<size_t> global_total_errors{0};
static std::atomic<size_t> global_total_discarded{0};
static std::atomic<size_t> global_pool_unavailable_batches{0};
static std::atomic<size_t> global_retry_exhausted_batches{0};
static std::atomic<size_t> global_insert_batches{0};
static std::atomic<size_t> global_upsert_batches{0};
static std::atomic<size_t> global_insert_discarded_tiles{0};
static std::atomic<size_t> global_upsert_discarded_tiles{0};

static std::unique_ptr<mongocxx::pool> global_pool;
static std::mutex pool_mutex;
static std::atomic<bool> pool_created{false};
static std::mutex global_state_mutex;
static bool collection_dropped = false;
static bool indexes_created = false;
static std::atomic<bool> first_connection_reported{false};

mongocxx::pool* MongoWriter::get_or_create_pool(const mongo_config &cfg) {
    if (pool_created.load(std::memory_order_acquire)) {
        return global_pool.get();
    }

    std::lock_guard<std::mutex> lock(pool_mutex);
    if (pool_created.load(std::memory_order_relaxed)) {
        return global_pool.get();
    }

    try {
        mongocxx::uri uri(cfg.uri());
        global_pool = std::make_unique<mongocxx::pool>(uri);
        pool_created.store(true, std::memory_order_release);

        extern int quiet;
        if (!quiet) {
            fprintf(stderr, "MongoDB connection pool created: %s.%s (maxPoolSize: %zu, uri: %s)\n",
                    cfg.dbname.c_str(), cfg.collection.c_str(), cfg.connection_pool_size,
                    cfg.safe_uri().c_str());
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "Error: Failed to create MongoDB connection pool: %s\n", e.what());
        ErrorLogger::instance().log_error(ErrorSource::MONGO_CONNECT, 0, 0, 0,
                                          "Failed to create MongoDB connection pool", e.what());
        return nullptr;
    }

    return global_pool.get();
}

MongoWriter* MongoWriter::get_thread_local_instance(const mongo_config &cfg) {
    if (!tls_mongo_writer) {
        tls_mongo_writer = std::make_unique<MongoWriter>(cfg);
        tls_mongo_writer->initialize_thread();
    }
    return tls_mongo_writer.get();
}

MongoWriter* MongoWriter::get_writer_instance(const mongo_config &cfg) {
    return get_thread_local_instance(cfg);
}

void MongoWriter::destroy_writer_instance() {
    destroy_current_thread_instance();
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

size_t MongoWriter::get_global_total_discarded() {
    return global_total_discarded.load();
}

size_t MongoWriter::get_global_pool_unavailable_batches() {
    return global_pool_unavailable_batches.load();
}

size_t MongoWriter::get_global_retry_exhausted_batches() {
    return global_retry_exhausted_batches.load();
}

size_t MongoWriter::get_global_insert_batches() {
    return global_insert_batches.load();
}

size_t MongoWriter::get_global_upsert_batches() {
    return global_upsert_batches.load();
}

size_t MongoWriter::get_global_insert_discarded_tiles() {
    return global_insert_discarded_tiles.load();
}

size_t MongoWriter::get_global_upsert_discarded_tiles() {
    return global_upsert_discarded_tiles.load();
}

void MongoWriter::destroy_current_thread_instance() {
    if (tls_mongo_writer) {
        tls_mongo_writer->flush_all();
        tls_mongo_writer->merge_stats_once();
        tls_mongo_writer.reset();
    }
}

bool MongoWriter::flush_current_thread_instance() {
    if (!tls_mongo_writer) {
        return true;
    }

    tls_mongo_writer->flush_all();
    return tls_mongo_writer->getCurrentBufferSize() == 0;
}

void MongoWriter::destroy_global_instance() {
    global_pool.reset();
    pool_created.store(false, std::memory_order_release);
    global_instance.reset();
    initialized.clear(std::memory_order_release);
    reset_global_runtime_state();
}

extern int quiet;

MongoWriter::MongoWriter(const mongo_config &cfg)
    : config(cfg)
{
    if (config.dbname.empty()) {
        throw std::runtime_error("MongoDB database name is required");
    }

    config.normalize();

    batch_buffer.reserve(config.batch_size);
    batch_coords.reserve(config.batch_size);
}

MongoWriter::~MongoWriter() noexcept
{
    close();
    merge_stats_once();
}

void MongoWriter::merge_stats_once() {
    if (stats_merged_) {
        return;
    }

    global_total_tiles += getTotalTilesWritten();
    global_total_batches += getTotalBatchesWritten();
    global_total_retries += getTotalRetries();
    global_total_errors += getTotalErrors();
    global_total_discarded += getTotalDiscardedTiles();
    global_pool_unavailable_batches += getPoolUnavailableBatches();
    global_retry_exhausted_batches += getRetryExhaustedBatches();
    global_insert_batches += getInsertBatches();
    global_upsert_batches += getUpsertBatches();
    global_insert_discarded_tiles += getInsertDiscardedTiles();
    global_upsert_discarded_tiles += getUpsertDiscardedTiles();
    stats_merged_ = true;
}

void MongoWriter::initialize_global()
{
    if (!initialized.test_and_set(std::memory_order_acquire)) {
        global_instance = std::make_unique<mongocxx::instance>();
    }
    reset_global_runtime_state();
}

void MongoWriter::reset_global_runtime_state() {
    std::lock_guard<std::mutex> lock(global_state_mutex);
    collection_dropped = false;
    indexes_created = false;
    global_total_tiles.store(0);
    global_total_batches.store(0);
    global_total_retries.store(0);
    global_total_errors.store(0);
    global_total_discarded.store(0);
    global_pool_unavailable_batches.store(0);
    global_retry_exhausted_batches.store(0);
    global_insert_batches.store(0);
    global_upsert_batches.store(0);
    global_insert_discarded_tiles.store(0);
    global_upsert_discarded_tiles.store(0);
    first_connection_reported.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> erased_lock(erased_zooms_mutex);
    erased_zooms.clear();
}

void MongoWriter::initialize_thread()
{
    auto pool = get_or_create_pool(config);
    if (!pool) {
        throw std::runtime_error("Failed to create MongoDB connection pool");
    }

    if (config.drop_collection_before_write) {
        bool should_drop = false;
        {
            std::lock_guard<std::mutex> lock(global_state_mutex);
            if (!collection_dropped) {
                collection_dropped = true;
                should_drop = true;
            }
        }
        if (should_drop) {
            try {
                auto inner_pool = get_or_create_pool(config);
                if (!inner_pool) return;
                auto client = inner_pool->acquire();
                auto collection = (*client)[config.dbname][config.collection];
                collection.drop();
                if (!quiet) {
                    fprintf(stderr, "MongoDB: dropped collection %s.%s\n",
                            config.dbname.c_str(), config.collection.c_str());
                }
            } catch (const std::exception &e) {
                fprintf(stderr, "Warning: failed to drop MongoDB collection %s.%s: %s\n",
                        config.dbname.c_str(), config.collection.c_str(), e.what());
            }
        }
    }

    if (config.create_indexes) {
        bool should_create_indexes = false;
        {
            std::lock_guard<std::mutex> lock(global_state_mutex);
            if (!indexes_created) {
                indexes_created = true;
                should_create_indexes = true;
            }
        }
        if (should_create_indexes) {
            try {
                auto inner_pool = get_or_create_pool(config);
                if (!inner_pool) return;
                auto client = inner_pool->acquire();
                auto collection = (*client)[config.dbname][config.collection];
                create_indexes_if_needed(collection);
            } catch (const std::exception &e) {
                fprintf(stderr, "Warning: failed to create indexes: %s\n", e.what());
            }
        }
    }

    if (config.enable_progress_report && !quiet && !first_connection_reported.exchange(true)) {
        fprintf(stderr, "MongoDB connected: %s.%s (pool: %zu, write concern: %d)\n",
                config.dbname.c_str(), config.collection.c_str(), config.connection_pool_size,
                static_cast<int>(config.write_concern_level));
    }
}

void MongoWriter::write_tile(int z, int x, int y, const char *data, size_t len)
{
    auto doc = bsoncxx::builder::stream::document{}
        << "z" << z
        << "x" << x
        << "y" << y
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
        while (!batch_buffer.empty()) {
            size_t before = batch_buffer.size();
            flush_batch();
            if (batch_buffer.size() == before) {
                break;
            }
        }
    } catch (const std::exception &e) {
        total_errors++;
    } catch (...) {
        total_errors++;
    }

    if (!batch_buffer.empty()) {
        total_errors++;
        size_t leftover_insert = 0;
        size_t leftover_upsert = 0;
        std::set<int> local_erased = get_erased_zooms_snapshot();
        for (const auto &coord : batch_coords) {
            if (local_erased.count(coord.z) > 0) {
                leftover_upsert++;
            } else {
                leftover_insert++;
            }
        }
        total_discarded_tiles += batch_buffer.size();
        total_insert_discarded_tiles += leftover_insert;
        total_upsert_discarded_tiles += leftover_upsert;
        ErrorLogger::instance().log_error(
            ErrorSource::MONGO_FLUSH,
            batch_coords.empty() ? 0 : batch_coords[0].z,
            batch_coords.empty() ? 0 : batch_coords[0].x,
            batch_coords.empty() ? 0 : batch_coords[0].y,
            "MongoDB flush_all left buffered tiles",
            "Tiles remained buffered after final flush attempt");
        batch_buffer.clear();
        batch_coords.clear();
    }
}

void MongoWriter::close() noexcept
{
    try {
        flush_all();
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

bool MongoWriter::should_use_upsert_for_zoom(int z) const
{
    std::lock_guard<std::mutex> lock(erased_zooms_mutex);
    return erased_zooms.count(z) > 0;
}

std::set<int> MongoWriter::get_erased_zooms_snapshot() const
{
    std::lock_guard<std::mutex> lock(erased_zooms_mutex);
    return erased_zooms;
}

void MongoWriter::flush_batch()
{
    if (batch_buffer.empty()) {
        return;
    }

    build_write_concern();

    std::set<int> local_erased = get_erased_zooms_snapshot();

    std::vector<bsoncxx::document::value> insert_buf, upsert_buf;
    std::vector<tile_coords> insert_coord, upsert_coord;

    for (size_t i = 0; i < batch_buffer.size(); i++) {
        if (local_erased.count(batch_coords[i].z) > 0) {
            upsert_buf.push_back(std::move(batch_buffer[i]));
            upsert_coord.push_back(batch_coords[i]);
        } else {
            insert_buf.push_back(std::move(batch_buffer[i]));
            insert_coord.push_back(batch_coords[i]);
        }
    }
    batch_buffer.clear();
    batch_coords.clear();

    if (!insert_buf.empty()) {
        flush_batch_with_retry(false, std::move(insert_buf), std::move(insert_coord));
    }
    if (!upsert_buf.empty()) {
        flush_batch_with_retry(true, std::move(upsert_buf), std::move(upsert_coord));
    }
}

static int exponential_backoff_with_jitter(int attempt) {
    static thread_local std::mt19937 gen(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    int base_ms = 100;
    int exp = std::min(attempt, 5);
    int max_wait = base_ms * (1 << exp);
    if (max_wait > 30000) max_wait = 30000;
    std::uniform_int_distribution<int> dist(0, max_wait / 2);
    return max_wait / 2 + dist(gen);
}

static bool should_log_retry_attempt(int attempts, int max_retries) {
    return attempts == 1 || attempts == max_retries || (attempts % 4 == 0);
}

void MongoWriter::flush_batch_with_retry(bool upsert_mode,
    std::vector<bsoncxx::document::value> batch_buf,
    std::vector<tile_coords> batch_coord)
{
    auto pool = get_or_create_pool(config);
    if (!pool) {
        total_errors++;
        total_failed_batches++;
        total_pool_unavailable_batches++;
        total_discarded_tiles += batch_buf.size();
        if (upsert_mode) {
            total_upsert_discarded_tiles += batch_buf.size();
        } else {
            total_insert_discarded_tiles += batch_buf.size();
        }
        ErrorLogger::instance().log_error(
            ErrorSource::MONGO_CONNECT,
            batch_coord.empty() ? 0 : batch_coord[0].z,
            batch_coord.empty() ? 0 : batch_coord[0].x,
            batch_coord.empty() ? 0 : batch_coord[0].y,
            upsert_mode ? "MongoDB pool unavailable for upsert batch" : "MongoDB pool unavailable for insert batch",
            "Discarded batch due to unavailable connection pool");
        return;
    }

    if (batch_buf.empty()) {
        return;
    }

    int attempts = 0;

    while (attempts < config.max_retries) {
        try {
            auto client = pool->acquire();
            auto collection = (*client)[config.dbname][config.collection];

            if (upsert_mode) {
                mongocxx::options::bulk_write bulk_opts;
                bulk_opts.ordered(false);
                bulk_opts.write_concern(cached_wc);
                auto bulk = collection.create_bulk_write(bulk_opts);

                for (size_t i = 0; i < batch_buf.size(); i++) {
                    bsoncxx::document::view view = batch_buf[i].view();
                    const auto &coord = batch_coord[i];

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
                views.reserve(batch_buf.size());
                for (const auto &doc : batch_buf) {
                    views.push_back(doc.view());
                }

                collection.insert_many(views, insert_opts);
            }

            flush_failure_rounds = 0;
            total_tiles_written += batch_buf.size();
            total_batches_written++;
            if (upsert_mode) {
                total_upsert_batches++;
            } else {
                total_insert_batches++;
            }

            return;

        } catch (const std::exception &e) {
            attempts++;
            total_errors++;

            if (!quiet && should_log_retry_attempt(attempts, config.max_retries)) {
                fprintf(stderr, "MongoDB flush_batch_%s failed (attempt %d/%d): %s\n",
                        upsert_mode ? "upsert" : "insert", attempts, config.max_retries, e.what());
            }

            if (attempts >= config.max_retries) {
                total_failed_batches++;
                flush_failure_rounds++;

                if (flush_failure_rounds >= 2) {
                    total_retry_exhausted_batches++;
                    fprintf(stderr, "Error: MongoDB flush_batch_%s failed after %d attempts x %zu rounds. %zu tiles discarded.\n",
                            upsert_mode ? "upsert" : "insert",
                            config.max_retries, flush_failure_rounds.load(), batch_buf.size());
                    total_discarded_tiles += batch_buf.size();
                    if (upsert_mode) {
                        total_upsert_discarded_tiles += batch_buf.size();
                    } else {
                        total_insert_discarded_tiles += batch_buf.size();
                    }
                    ErrorLogger::instance().log_error(
                        ErrorSource::MONGO_FLUSH,
                        batch_coord.empty() ? 0 : batch_coord[0].z,
                        batch_coord.empty() ? 0 : batch_coord[0].x,
                        batch_coord.empty() ? 0 : batch_coord[0].y,
                        upsert_mode ? "MongoDB upsert batch discarded after retries" : "MongoDB insert batch discarded after retries",
                        "Batch discarded after repeated flush failures");
                    flush_failure_rounds = 0;
                } else {
                    fprintf(stderr, "Error: MongoDB flush_batch_%s failed after %d attempts (round %zu). %zu tiles remain in buffer.\n",
                            upsert_mode ? "upsert" : "insert",
                            config.max_retries, flush_failure_rounds.load(), batch_buf.size());
                    for (size_t i = 0; i < batch_buf.size(); i++) {
                        batch_buffer.push_back(std::move(batch_buf[i]));
                        batch_coords.push_back(batch_coord[i]);
                    }
                }
                return;
            }

            total_retries++;
            int wait_ms = exponential_backoff_with_jitter(attempts);
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        }
    }
}

void MongoWriter::create_indexes_if_needed(mongocxx::collection &collection)
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
        auto pool = get_or_create_pool(config);
        if (!pool) return;
        auto client = pool->acquire();
        auto collection = (*client)[config.dbname][config.collection];

        auto filter = bsoncxx::builder::stream::document{}
            << "z" << z
            << bsoncxx::builder::stream::finalize;

        auto result = collection.delete_many(filter.view());

        if (!quiet) {
            fprintf(stderr, "MongoDB: deleted %d tiles at z=%d\n",
                    static_cast<int>(result->deleted_count()), z);
        }

        {
            std::lock_guard<std::mutex> lock(erased_zooms_mutex);
            erased_zooms.insert(z);
        }
        if (!quiet) {
            fprintf(stderr, "MongoDB: zoom %d marked for upsert mode\n", z);
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

void MongoWriter::write_metadata_bson(const struct metadata &meta)
{
    try {
        auto pool = get_or_create_pool(config);
        if (!pool) return;
        auto client = pool->acquire();
        auto db = (*client)[config.dbname];
        std::string meta_collection_name = config.collection + "_metadata";
        auto meta_collection = db[meta_collection_name];

        meta_collection.drop();

        auto doc = bsoncxx::builder::basic::document{};

        doc.append(bsoncxx::builder::basic::kvp("name", meta.name));
        doc.append(bsoncxx::builder::basic::kvp("description", meta.description));
        doc.append(bsoncxx::builder::basic::kvp("version", meta.version));
        doc.append(bsoncxx::builder::basic::kvp("type", meta.type));
        doc.append(bsoncxx::builder::basic::kvp("format", meta.format));
        doc.append(bsoncxx::builder::basic::kvp("minzoom", meta.minzoom));
        doc.append(bsoncxx::builder::basic::kvp("maxzoom", meta.maxzoom));

        doc.append(bsoncxx::builder::basic::kvp("bounds",
            [&meta](bsoncxx::builder::basic::sub_array sub) {
                sub.append(meta.minlon, meta.minlat, meta.maxlon, meta.maxlat);
            }));

        doc.append(bsoncxx::builder::basic::kvp("center",
            [&meta](bsoncxx::builder::basic::sub_array sub) {
                sub.append(meta.center_lon, meta.center_lat, meta.center_z);
            }));

        if (!meta.attribution.empty()) {
            doc.append(bsoncxx::builder::basic::kvp("attribution", meta.attribution));
        }
        if (!meta.generator.empty()) {
            doc.append(bsoncxx::builder::basic::kvp("generator", meta.generator));
        }
        if (!meta.vector_layers_json.empty()) {
            doc.append(bsoncxx::builder::basic::kvp("vector_layers", meta.vector_layers_json));
        }
        if (!meta.tilestats_json.empty()) {
            doc.append(bsoncxx::builder::basic::kvp("tilestats", meta.tilestats_json));
        }

        doc.append(bsoncxx::builder::basic::kvp("collection", config.collection));
        doc.append(bsoncxx::builder::basic::kvp("timestamp",
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()));

        auto doc_value = doc.extract();

        meta_collection.insert_one(doc_value.view());

        if (!quiet) {
            fprintf(stderr, "MongoDB: wrote metadata to %s.%s\n",
                    config.dbname.c_str(), meta_collection_name.c_str());
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "Warning: failed to write MongoDB metadata (bson): %s\n", e.what());
    }
}
