#ifndef POSTGIS_MANAGER_HPP
#define POSTGIS_MANAGER_HPP

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include "postgis.hpp"
#include "serial.hpp"

namespace PostGIS {

bool validate_config(const postgis_config& cfg);
std::string get_validation_error();

class ParallelReader {
public:
    ParallelReader(const postgis_config& cfg, size_t num_threads);
    ~ParallelReader();

    bool read_parallel(std::vector<struct serialization_state>& sst,
                      size_t layer, const std::string& layername);

    size_t get_total_features() const { return total_features.load(); }
    size_t get_total_parse_errors() const { return total_parse_errors.load(); }

private:
    postgis_config config;
    size_t num_threads;
    std::atomic<size_t> total_features{0};
    std::atomic<size_t> total_parse_errors{0};
};

}

#endif
