#ifndef MONGO_MANAGER_HPP
#define MONGO_MANAGER_HPP

#include <string>
#include <optional>
#include <cstddef>
#include "mongo.hpp"

namespace MongoDB {

std::optional<std::string> validate_config(const mongo_config& cfg);

size_t suggest_batch_size(size_t estimated_tiles);
size_t estimate_tile_count(size_t feature_count, int min_zoom, int max_zoom);

void initialize_global();
void cleanup_global();
void shutdown_global();

struct GlobalStats {
    size_t total_tiles;
    size_t total_batches;
    size_t total_retries;
    size_t total_errors;
    size_t total_discarded;
    size_t pool_unavailable_batches;
    size_t retry_exhausted_batches;
    size_t insert_batches;
    size_t upsert_batches;
    size_t insert_discarded_tiles;
    size_t upsert_discarded_tiles;
};

GlobalStats get_global_stats();
void print_stats(const GlobalStats& stats, bool quiet = false);

} // namespace MongoDB

#endif // MONGO_MANAGER_HPP
