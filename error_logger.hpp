#ifndef ERROR_LOGGER_HPP
#define ERROR_LOGGER_HPP

#include <string>
#include <cstddef>
#include <mutex>
#include <atomic>

enum class ErrorSource {
    POSTGIS_READ,
    POSTGIS_PARSE,
    MONGO_WRITE,
    MONGO_FLUSH,
    MONGO_CONNECT,
    GENERAL
};

struct ErrorStats {
    size_t postgis_read_errors;
    size_t postgis_parse_errors;
    size_t mongo_write_errors;
    size_t mongo_flush_errors;
    size_t mongo_connect_errors;
    size_t general_errors;
    size_t total_errors;
};

class ErrorLogger {
public:
    static ErrorLogger& instance();

    bool initialize(const std::string& exec_path);
    void close();

    void log_error(ErrorSource source, int z, int x, int y,
                   const std::string& message,
                   const std::string& detail = "");

    void log_parse_error(int row, const std::string& geometry_type,
                         const std::string& message,
                         const std::string& wkb_hex_preview = "");

    void log_mongo_error(int z, int x, int y,
                         const std::string& operation,
                         const std::string& message);

    ErrorStats get_stats() const;
    void print_summary(bool quiet_mode = false) const;

    bool is_initialized() const { return db_ != nullptr; }

private:
    ErrorLogger() = default;
    ~ErrorLogger();

    ErrorLogger(const ErrorLogger&) = delete;
    ErrorLogger& operator=(const ErrorLogger&) = delete;

    void create_tables();
    void begin_transaction();
    void commit_transaction();

    void* db_ = nullptr;
    std::mutex mutex_;
    std::string db_path_;
    size_t uncommitted_ = 0;
    static constexpr size_t COMMIT_INTERVAL = 50;

    mutable std::atomic<size_t> postgis_read_errors_{0};
    mutable std::atomic<size_t> postgis_parse_errors_{0};
    mutable std::atomic<size_t> mongo_write_errors_{0};
    mutable std::atomic<size_t> mongo_flush_errors_{0};
    mutable std::atomic<size_t> mongo_connect_errors_{0};
    mutable std::atomic<size_t> general_errors_{0};
};

#endif
