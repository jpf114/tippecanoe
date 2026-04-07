#include "postgis_manager.hpp"
#include "config.hpp"
#include <cstring>
#include <cstdio>

namespace PostGIS {

static std::string validation_error;

bool validate_config(const postgis_config& cfg) {
    validation_error.clear();
    
    if (cfg.host.empty()) {
        validation_error = "PostGIS host is required";
        return false;
    }
    
    if (cfg.dbname.empty()) {
        validation_error = "PostGIS database name is required";
        return false;
    }
    
    if (cfg.user.empty()) {
        validation_error = "PostGIS user is required";
        return false;
    }
    
    if (cfg.password.empty()) {
        validation_error = "PostGIS password is required";
        return false;
    }
    
    if (cfg.table.empty() && cfg.sql.empty()) {
        validation_error = "Either PostGIS table or SQL query is required";
        return false;
    }
    
    if (cfg.geometry_field.empty()) {
        validation_error = "PostGIS geometry field is required";
        return false;
    }
    
    return true;
}

std::string get_validation_error() {
    return validation_error;
}

ParallelReader::ParallelReader(const postgis_config& cfg, size_t threads)
    : config(cfg), num_threads(threads) {
}

ParallelReader::~ParallelReader() {
}

bool ParallelReader::read_parallel(std::vector<struct serialization_state>& sst, 
                                   size_t layer, const std::string& layername) {
    if (!config.pk_field.empty()) {
        return read_with_pk_range(sst, layer, layername);
    } else {
        return read_single_thread(sst, layer, layername);
    }
}

bool ParallelReader::read_with_pk_range(std::vector<struct serialization_state>& sst, 
                                        size_t layer, const std::string& layername) {
    PostGISReader reader(config);
    if (!reader.connect()) {
        fprintf(stderr, "Failed to connect to PostGIS database\n");
        return false;
    }
    
    long long min_pk = 0, max_pk = 0;
    if (!reader.get_pk_range(min_pk, max_pk)) {
        fprintf(stderr, "Failed to get PK range, falling back to single thread.\n");
        config.pk_field.clear();
        return read_single_thread(sst, layer, layername);
    }
    
    DEBUG_LOG("Parallel Fetch enabled over PK %s: Range [%lld, %lld]", 
             config.pk_field.c_str(), min_pk, max_pk);
    
    long long pk_range = max_pk - min_pk;
    if (pk_range < 0) {
        return read_single_thread(sst, layer, layername);
    }
    
    max_pk += 1;
    pk_range += 1;
    
    long long chunk = pk_range / num_threads;
    if (chunk == 0) chunk = 1;
    
    std::vector<std::thread> threads;
    std::atomic<size_t> thread_errors{0};
    
    for (size_t i = 0; i < num_threads; i++) {
        long long t_min = min_pk + i * chunk;
        long long t_max = (i == num_threads - 1) ? max_pk : (t_min + chunk);
        
        if (t_min >= max_pk) break;
        
        threads.emplace_back([&, i, t_min, t_max]() {
            PostGISReader t_reader(config);
            if (!t_reader.read_features(sst, layer, layername, t_min, t_max, true, i)) {
                fprintf(stderr, "Thread %zu failed to read features\n", i);
                thread_errors.fetch_add(1);
            }
            total_features.fetch_add(t_reader.getTotalFeaturesProcessed());
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    return thread_errors.load() == 0;
}

bool ParallelReader::read_single_thread(std::vector<struct serialization_state>& sst, 
                                        size_t layer, const std::string& layername) {
    PostGISReader reader(config);
    if (!reader.connect()) {
        fprintf(stderr, "Failed to connect to PostGIS database\n");
        return false;
    }
    
    if (!reader.read_features(sst, layer, layername)) {
        fprintf(stderr, "Failed to read features from PostGIS\n");
        return false;
    }
    
    total_features = reader.getTotalFeaturesProcessed();
    return true;
}

} // namespace PostGIS
