#include "mongo_manager.hpp"
#include "config.hpp"
#include <cstdio>
#include <climits>

namespace MongoDB {

std::optional<std::string> validate_config(const mongo_config& cfg) {
    if (cfg.host.empty()) {
        return "MongoDB host is required";
    }

    if (cfg.port == 0) {
        return "MongoDB port is required";
    }

    if (cfg.dbname.empty()) {
        return "MongoDB database name is required (for example: --mongo tiles:collection or --mongo-dbname tiles)";
    }

    if (cfg.collection.empty()) {
        return "MongoDB collection is required (for example: --mongo tiles:collection or --mongo-collection collection)";
    }

    if (cfg.username.empty() != cfg.password.empty()) {
        return "MongoDB username and password must be provided together";
    }

    return std::nullopt;
}

size_t suggest_batch_size(size_t estimated_tiles) {
    if (estimated_tiles < MONGO_BATCH_TIER_1) {
        return MONGO_BATCH_SIZE_TIER_1;
    } else if (estimated_tiles < MONGO_BATCH_TIER_2) {
        return MONGO_BATCH_SIZE_TIER_2;
    } else if (estimated_tiles < MONGO_BATCH_TIER_3) {
        return MONGO_BATCH_SIZE_TIER_3;
    } else if (estimated_tiles < MONGO_BATCH_TIER_4) {
        return MONGO_BATCH_SIZE_TIER_4;
    } else {
        return MONGO_BATCH_SIZE_TIER_5;
    }
}

size_t estimate_tile_count(size_t feature_count, int min_zoom, int max_zoom) {
    if (feature_count == 0 || min_zoom < 0 || max_zoom < 0 || min_zoom > max_zoom) {
        return 0;
    }

    size_t total_tiles = 0;
    int zoom_range = max_zoom - min_zoom + 1;

    for (int z = min_zoom; z <= max_zoom; z++) {
        if (z >= 16) {
            total_tiles += feature_count;
            continue;
        }
        size_t tiles_at_zoom = (2 * z < 64) ? (1ULL << (2 * z)) : ULLONG_MAX;
        double coverage_ratio = std::min(1.0, feature_count * 4.0 / static_cast<double>(tiles_at_zoom));
        total_tiles += static_cast<size_t>(tiles_at_zoom * coverage_ratio * 0.3);
    }

    total_tiles = std::max(total_tiles, feature_count * static_cast<size_t>(zoom_range));

    return total_tiles;
}

void initialize_global() {
    DEBUG_LOG("Initializing MongoDB global instance...");
    MongoWriter::initialize_global();
    DEBUG_LOG("MongoDB global initialization done.");
}

void cleanup_global() {
    DEBUG_LOG("Cleaning up MongoDB thread-local instances...");
    MongoWriter::destroy_current_thread_instance();
    DEBUG_LOG("MongoDB cleanup done.");
}

void shutdown_global() {
    DEBUG_LOG("Shutting down MongoDB global instance...");
    MongoWriter::destroy_writer_instance();
    MongoWriter::destroy_global_instance();
    DEBUG_LOG("MongoDB global instance shut down.");
}

GlobalStats get_global_stats() {
    GlobalStats stats;
    stats.total_tiles = MongoWriter::get_global_total_tiles();
    stats.total_batches = MongoWriter::get_global_total_batches();
    stats.total_retries = MongoWriter::get_global_total_retries();
    stats.total_errors = MongoWriter::get_global_total_errors();
    stats.total_discarded = MongoWriter::get_global_total_discarded();
    stats.pool_unavailable_batches = MongoWriter::get_global_pool_unavailable_batches();
    stats.retry_exhausted_batches = MongoWriter::get_global_retry_exhausted_batches();
    stats.insert_batches = MongoWriter::get_global_insert_batches();
    stats.upsert_batches = MongoWriter::get_global_upsert_batches();
    stats.insert_discarded_tiles = MongoWriter::get_global_insert_discarded_tiles();
    stats.upsert_discarded_tiles = MongoWriter::get_global_upsert_discarded_tiles();
    return stats;
}

void print_stats(const GlobalStats& stats, bool quiet) {
    if (!quiet) {
        fprintf(stderr, "\nMongoDB Statistics:\n");
        fprintf(stderr, "  Total tiles written: %zu\n", stats.total_tiles);
        fprintf(stderr, "  Total batches written: %zu\n", stats.total_batches);
        fprintf(stderr, "  Total retries: %zu\n", stats.total_retries);
        fprintf(stderr, "  Total errors: %zu\n", stats.total_errors);
        fprintf(stderr, "  Total tiles discarded: %zu\n", stats.total_discarded);
        fprintf(stderr, "  Pool-unavailable batches: %zu\n", stats.pool_unavailable_batches);
        fprintf(stderr, "  Retry-exhausted batches: %zu\n", stats.retry_exhausted_batches);
        fprintf(stderr, "  Insert batches written: %zu\n", stats.insert_batches);
        fprintf(stderr, "  Upsert batches written: %zu\n", stats.upsert_batches);
        fprintf(stderr, "  Insert discarded tiles: %zu\n", stats.insert_discarded_tiles);
        fprintf(stderr, "  Upsert discarded tiles: %zu\n", stats.upsert_discarded_tiles);
    }
}

} // namespace MongoDB
