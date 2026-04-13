#include "postgis_manager.hpp"
#include "config.hpp"
#include "error_logger.hpp"
#include <cstring>
#include <cstdio>
#include <algorithm>

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
        validation_error = "PostGIS password is required (use --postgis-password if empty)";
        return false;
    }

    if (cfg.table.empty() && cfg.sql.empty()) {
        validation_error = "Either PostGIS table or SQL query is required";
        return false;
    }

    if (cfg.geometry_field.empty() && cfg.sql.empty()) {
        validation_error = "PostGIS geometry field is required (unless using custom SQL)";
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
    if (num_threads <= 1) {
        PostGISReader reader(config);
        if (!reader.connect()) {
            fprintf(stderr, "Failed to connect to PostGIS database\n");
            ErrorLogger::instance().log_error(ErrorSource::POSTGIS_READ, 0, 0, 0,
                                              "Failed to connect to PostGIS database");
            reader.disconnect();
            return false;
        }

        if (!reader.read_features(sst, layer, layername, 0, 1)) {
            fprintf(stderr, "Failed to read features from PostGIS\n");
            reader.disconnect();
            return false;
        }

        total_features = reader.getTotalFeaturesProcessed();
        total_parse_errors = reader.getParseErrors();
        reader.disconnect();
        return true;
    }

    fprintf(stderr, "Starting parallel read with %zu threads (hash-based ctid sharding)\n", num_threads);

    std::vector<std::thread> threads;
    std::atomic<size_t> thread_errors{0};

    for (size_t i = 0; i < num_threads; i++) {
        threads.emplace_back([&, i]() {
            PostGISReader t_reader(config);
            if (!t_reader.connect()) {
                fprintf(stderr, "Thread %zu failed to connect to PostGIS\n", i);
                ErrorLogger::instance().log_error(ErrorSource::POSTGIS_READ, 0, 0, 0,
                                                  "Thread " + std::to_string(i) + ": connection failed");
                thread_errors.fetch_add(1);
                t_reader.disconnect();
                return;
            }

            if (!t_reader.read_features(sst, layer, layername, i, num_threads)) {
                fprintf(stderr, "Thread %zu failed to read features\n", i);
                thread_errors.fetch_add(1);
                t_reader.disconnect();
                return;
            }

            total_features.fetch_add(t_reader.getTotalFeaturesProcessed());
            total_parse_errors.fetch_add(t_reader.getParseErrors());
            t_reader.disconnect();
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    if (thread_errors.load() > 0) {
        fprintf(stderr, "Warning: %zu of %zu threads encountered errors during parallel read\n",
                thread_errors.load(), num_threads);
    }

    fprintf(stderr, "Parallel read complete: %zu features, %zu parse errors\n",
            total_features.load(), total_parse_errors.load());

    return thread_errors.load() == 0;
}

}
