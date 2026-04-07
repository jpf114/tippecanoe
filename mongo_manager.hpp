#ifndef MONGO_MANAGER_HPP
#define MONGO_MANAGER_HPP

#include <string>
#include <cstddef>
#include "mongo.hpp"

namespace MongoDB {

bool validate_config(const mongo_config& cfg);
std::string get_validation_error();

size_t suggest_batch_size(size_t estimated_tiles);
size_t estimate_tile_count(size_t feature_count);

void initialize_global();
void cleanup_global();

struct GlobalStats {
    size_t total_tiles;
    size_t total_batches;
    size_t total_retries;
    size_t total_errors;
};

GlobalStats get_global_stats();
void print_stats(const GlobalStats& stats, bool quiet = false);

} // namespace MongoDB

#endif // MONGO_MANAGER_HPP
