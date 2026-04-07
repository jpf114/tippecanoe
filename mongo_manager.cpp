#include "mongo_manager.hpp"
#include "config.hpp"
#include <cstdio>

namespace MongoDB {

static std::string validation_error;

bool validate_config(const mongo_config& cfg) {
    validation_error.clear();
    
    if (cfg.host.empty()) {
        validation_error = "MongoDB host is required";
        return false;
    }
    
    if (cfg.port == 0) {
        validation_error = "MongoDB port is required";
        return false;
    }
    
    if (cfg.dbname.empty()) {
        validation_error = "MongoDB database name is required";
        return false;
    }
    
    if (cfg.collection.empty()) {
        validation_error = "MongoDB collection is required";
        return false;
    }
    
    if (cfg.username.empty()) {
        validation_error = "MongoDB username is required";
        return false;
    }
    
    if (cfg.password.empty()) {
        validation_error = "MongoDB password is required";
        return false;
    }
    
    if (cfg.auth_source.empty()) {
        validation_error = "MongoDB auth source is required";
        return false;
    }
    
    return true;
}

std::string get_validation_error() {
    return validation_error;
}

size_t suggest_batch_size(size_t estimated_tiles) {
    if (estimated_tiles < 1000) {
        return 50;
    } else if (estimated_tiles < 10000) {
        return 100;
    } else if (estimated_tiles < 100000) {
        return 200;
    } else {
        return 500;
    }
}

size_t estimate_tile_count(size_t feature_count) {
    return feature_count * 2;
}

void initialize_global() {
    DEBUG_LOG("Initializing MongoDB global instance...");
    MongoWriter::initialize_global();
    DEBUG_LOG("MongoDB global initialization done.");
}

void cleanup_global() {
    DEBUG_LOG("Cleaning up MongoDB thread local instances...");
    MongoWriter::destroy_thread_local_instances();
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
