#include "postgis_manager.hpp"
#include "config.hpp"
#include "error_logger.hpp"
#include <cstring>
#include <cstdio>
#include <algorithm>

extern int quiet;

namespace PostGIS {

static std::string validation_error;

static bool should_emit_parallel_read_logs(const postgis_config &cfg) {
    return cfg.enable_progress_report && !quiet;
}

bool validate_config(const postgis_config& cfg) {
    validation_error.clear();

    if (cfg.dbname.empty()) {
        validation_error = "PostGIS database name is required (for example: --postgis gis or --postgis-host/--postgis-dbname)";
        return false;
    }

    if (!cfg.has_input_source()) {
        validation_error = "Either PostGIS table or SQL query is required (--postgis-table or --postgis-sql)";
        return false;
    }

    if (cfg.requires_explicit_geometry_field()) {
        validation_error = "PostGIS geometry field is required (use --postgis-geometry-field unless your custom SQL already handles it)";
        return false;
    }

    if (cfg.requires_selected_columns_for_best_effort()) {
        validation_error = "--postgis-columns-best-effort requires --postgis-columns";
        return false;
    }

    if (cfg.requires_shard_key()) {
        validation_error = "--postgis-shard-key is required when --postgis-shard-mode=" + cfg.effective_shard_mode();
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

    if (should_emit_parallel_read_logs(config)) {
        fprintf(stderr, "Starting parallel read with %zu threads", num_threads);
        if (config.should_report_shard_strategy()) {
            std::string shard_mode = config.effective_shard_mode();
            fprintf(stderr, " (shard mode: %s", shard_mode.c_str());
            if (!config.shard_key.empty()) {
                fprintf(stderr, ", shard key: %s", config.shard_key.c_str());
            }
            fprintf(stderr, ")");
        }
        fprintf(stderr, "\n");
    }

    std::vector<std::thread> threads;
    std::atomic<size_t> thread_errors{0};

    for (size_t i = 0; i < num_threads; i++) {
        threads.emplace_back([&, i]() {
            try {
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
            } catch (const std::exception& e) {
                fprintf(stderr, "Thread %zu exception: %s\n", i, e.what());
                ErrorLogger::instance().log_error(ErrorSource::POSTGIS_READ, 0, 0, 0,
                                                  std::string("Thread ") + std::to_string(i) + " exception: " + e.what());
                thread_errors.fetch_add(1);
            } catch (...) {
                fprintf(stderr, "Thread %zu unknown exception\n", i);
                ErrorLogger::instance().log_error(ErrorSource::POSTGIS_READ, 0, 0, 0,
                                                  std::string("Thread ") + std::to_string(i) + " unknown exception");
                thread_errors.fetch_add(1);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    if (thread_errors.load() > 0 && !quiet) {
        fprintf(stderr, "Warning: %zu of %zu threads encountered errors during parallel read\n",
                thread_errors.load(), num_threads);
    }

    if (should_emit_parallel_read_logs(config)) {
        fprintf(stderr, "Parallel read complete: %zu features, %zu parse errors\n",
                total_features.load(), total_parse_errors.load());
    }

    return thread_errors.load() == 0;
}

}
