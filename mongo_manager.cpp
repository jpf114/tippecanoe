#include "mongo_manager.hpp"
#include "config.hpp"
#include <cstdio>

namespace MongoDB {

std::optional<std::string> validate_config(const mongo_config& cfg) {
    if (cfg.host.empty()) {
        return "MongoDB host is required";
    }
    
    if (cfg.port == 0) {
        return "MongoDB port is required";
    }
    
    if (cfg.dbname.empty()) {
        return "MongoDB database name is required";
    }
    
    if (cfg.collection.empty()) {
        return "MongoDB collection is required";
    }
    
    if (cfg.username.empty()) {
        return "MongoDB username is required";
    }
    
    if (cfg.password.empty()) {
        return "MongoDB password is required";
    }
    
    if (cfg.auth_source.empty()) {
        return "MongoDB auth source is required";
    }
    
    return std::nullopt;
}

size_t suggest_batch_size(size_t estimated_tiles) {
    if (estimated_tiles < 10000) {
        return 100;
    } else if (estimated_tiles < 100000) {
        return 200;
    } else if (estimated_tiles < 500000) {
        return 500;
    } else if (estimated_tiles < 2000000) {
        return 800;
    } else {
        return 1000;
    }
}

size_t estimate_tile_count(size_t feature_count, int min_zoom, int max_zoom) {
    if (feature_count == 0 || min_zoom > max_zoom) {
        return 0;
    }
    
    size_t total_tiles = 0;
    int zoom_range = max_zoom - min_zoom + 1;
    
    for (int z = min_zoom; z <= max_zoom; z++) {
        size_t tiles_at_zoom = 1ULL << (2 * z);
        double coverage_ratio = std::min(1.0, feature_count * 4.0 / tiles_at_zoom);
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
    DEBUG_LOG("Cleaning up MongoDB thread local instances...");
    MongoWriter::destroy_current_thread_instance();
    DEBUG_LOG("MongoDB cleanup done.");
}

GlobalStats get_global_stats() {
    GlobalStats stats;
    stats.total_tiles = MongoWriter::get_global_total_tiles();
    stats.total_batches = MongoWriter::get_global_total_batches();
    stats.total_retries = MongoWriter::get_global_total_retries();
    stats.total_errors = MongoWriter::get_global_total_errors();
    return stats;
}

void print_stats(const GlobalStats& stats, bool quiet) {
    if (!quiet) {
        fprintf(stderr, "\nMongoDB Statistics:\n");
        fprintf(stderr, "  Total tiles written: %zu\n", stats.total_tiles);
        fprintf(stderr, "  Total batches written: %zu\n", stats.total_batches);
        fprintf(stderr, "  Total retries: %zu\n", stats.total_retries);
        fprintf(stderr, "  Total errors: %zu\n", stats.total_errors);
    }
}

} // namespace MongoDB
