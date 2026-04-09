#include "error_logger.hpp"
#include "config.hpp"
#include <sqlite3.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <ctime>

extern int quiet;

static std::string current_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t_now));
    return std::string(buf);
}

ErrorLogger& ErrorLogger::instance() {
    static ErrorLogger logger;
    return logger;
}

ErrorLogger::~ErrorLogger() {
    close();
}

bool ErrorLogger::initialize(const std::string& exec_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (db_ != nullptr) {
        return true;
    }

    db_path_ = exec_path + "/tile_errors.db";

    int rc = sqlite3_open(db_path_.c_str(), reinterpret_cast<sqlite3**>(&db_));
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error: Cannot open error log database %s: %s\n",
                db_path_.c_str(), sqlite3_errmsg(reinterpret_cast<sqlite3*>(db_)));
        db_ = nullptr;
        return false;
    }

    char* err_msg = nullptr;
    rc = sqlite3_exec(reinterpret_cast<sqlite3*>(db_),
                      "PRAGMA journal_mode=WAL;"
                      "PRAGMA synchronous=NORMAL;"
                      "PRAGMA busy_timeout=5000;",
                      nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Warning: Failed to set SQLite pragmas: %s\n", err_msg);
        sqlite3_free(err_msg);
    }

    create_tables();

    if (!quiet) {
        fprintf(stderr, "Error logger initialized: %s\n", db_path_.c_str());
    }

    return true;
}

void ErrorLogger::create_tables() {
    auto* db = reinterpret_cast<sqlite3*>(db_);

    const char* create_postgis_errors =
        "CREATE TABLE IF NOT EXISTS postgis_errors ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "timestamp TEXT NOT NULL,"
        "row_num INTEGER,"
        "geometry_type TEXT,"
        "error_message TEXT NOT NULL,"
        "data_preview TEXT"
        ");";

    const char* create_mongo_errors =
        "CREATE TABLE IF NOT EXISTS mongo_errors ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "timestamp TEXT NOT NULL,"
        "z INTEGER,"
        "x INTEGER,"
        "y INTEGER,"
        "operation TEXT NOT NULL,"
        "error_message TEXT NOT NULL"
        ");";

    const char* create_general_errors =
        "CREATE TABLE IF NOT EXISTS general_errors ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "timestamp TEXT NOT NULL,"
        "source TEXT NOT NULL,"
        "error_message TEXT NOT NULL"
        ");";

    char* err_msg = nullptr;
    sqlite3_exec(db, create_postgis_errors, nullptr, nullptr, &err_msg);
    if (err_msg) { sqlite3_free(err_msg); }

    sqlite3_exec(db, create_mongo_errors, nullptr, nullptr, &err_msg);
    if (err_msg) { sqlite3_free(err_msg); }

    sqlite3_exec(db, create_general_errors, nullptr, nullptr, &err_msg);
    if (err_msg) { sqlite3_free(err_msg); }
}

void ErrorLogger::begin_transaction() {
    auto* db = reinterpret_cast<sqlite3*>(db_);
    sqlite3_exec(db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
}

void ErrorLogger::commit_transaction() {
    auto* db = reinterpret_cast<sqlite3*>(db_);
    sqlite3_exec(db, "COMMIT TRANSACTION", nullptr, nullptr, nullptr);
    uncommitted_ = 0;
}

void ErrorLogger::log_error(ErrorSource source, int z, int x, int y,
                            const std::string& message,
                            const std::string& detail) {
    switch (source) {
        case ErrorSource::POSTGIS_READ:  postgis_read_errors_.fetch_add(1); break;
        case ErrorSource::POSTGIS_PARSE: postgis_parse_errors_.fetch_add(1); break;
        case ErrorSource::MONGO_WRITE:   mongo_write_errors_.fetch_add(1); break;
        case ErrorSource::MONGO_FLUSH:   mongo_flush_errors_.fetch_add(1); break;
        case ErrorSource::MONGO_CONNECT: mongo_connect_errors_.fetch_add(1); break;
        case ErrorSource::GENERAL:       general_errors_.fetch_add(1); break;
    }

    if (db_ == nullptr) return;

    std::lock_guard<std::mutex> lock(mutex_);

    if (uncommitted_ == 0) {
        begin_transaction();
    }

    auto* db = reinterpret_cast<sqlite3*>(db_);
    const char* sql = nullptr;

    if (source == ErrorSource::POSTGIS_READ || source == ErrorSource::POSTGIS_PARSE) {
        sql = "INSERT INTO postgis_errors (timestamp, row_num, geometry_type, error_message, data_preview) VALUES (?, ?, ?, ?, ?)";
    } else if (source == ErrorSource::MONGO_WRITE || source == ErrorSource::MONGO_FLUSH || source == ErrorSource::MONGO_CONNECT) {
        sql = "INSERT INTO mongo_errors (timestamp, z, x, y, operation, error_message) VALUES (?, ?, ?, ?, ?, ?)";
    } else {
        sql = "INSERT INTO general_errors (timestamp, source, error_message) VALUES (?, ?, ?)";
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    std::string ts = current_timestamp();
    sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);

    if (source == ErrorSource::POSTGIS_READ || source == ErrorSource::POSTGIS_PARSE) {
        sqlite3_bind_int(stmt, 2, z);
        sqlite3_bind_text(stmt, 3, "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, message.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, detail.c_str(), -1, SQLITE_TRANSIENT);
    } else if (source == ErrorSource::MONGO_WRITE || source == ErrorSource::MONGO_FLUSH || source == ErrorSource::MONGO_CONNECT) {
        sqlite3_bind_int(stmt, 2, z);
        sqlite3_bind_int(stmt, 3, x);
        sqlite3_bind_int(stmt, 4, y);
        const char* op_name = "";
        switch (source) {
            case ErrorSource::MONGO_WRITE:   op_name = "write_tile"; break;
            case ErrorSource::MONGO_FLUSH:   op_name = "flush_batch"; break;
            case ErrorSource::MONGO_CONNECT: op_name = "connect"; break;
            default: break;
        }
        sqlite3_bind_text(stmt, 5, op_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, message.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_text(stmt, 2, "general", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, message.c_str(), -1, SQLITE_TRANSIENT);
    }

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    uncommitted_++;
    if (uncommitted_ >= COMMIT_INTERVAL) {
        commit_transaction();
    }
}

void ErrorLogger::log_parse_error(int row, const std::string& geometry_type,
                                  const std::string& message,
                                  const std::string& wkb_hex_preview) {
    postgis_parse_errors_.fetch_add(1);

    if (db_ == nullptr) return;

    std::lock_guard<std::mutex> lock(mutex_);

    if (uncommitted_ == 0) {
        begin_transaction();
    }

    auto* db = reinterpret_cast<sqlite3*>(db_);
    const char* sql =
        "INSERT INTO postgis_errors (timestamp, row_num, geometry_type, error_message, data_preview) "
        "VALUES (?, ?, ?, ?, ?)";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    std::string ts = current_timestamp();
    sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, row);
    sqlite3_bind_text(stmt, 3, geometry_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, wkb_hex_preview.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    uncommitted_++;
    if (uncommitted_ >= COMMIT_INTERVAL) {
        commit_transaction();
    }
}

void ErrorLogger::log_mongo_error(int z, int x, int y,
                                  const std::string& operation,
                                  const std::string& message) {
    mongo_write_errors_.fetch_add(1);

    if (db_ == nullptr) return;

    std::lock_guard<std::mutex> lock(mutex_);

    if (uncommitted_ == 0) {
        begin_transaction();
    }

    auto* db = reinterpret_cast<sqlite3*>(db_);
    const char* sql =
        "INSERT INTO mongo_errors (timestamp, z, x, y, operation, error_message) "
        "VALUES (?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    std::string ts = current_timestamp();
    sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, z);
    sqlite3_bind_int(stmt, 3, x);
    sqlite3_bind_int(stmt, 4, y);
    sqlite3_bind_text(stmt, 5, operation.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, message.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    uncommitted_++;
    if (uncommitted_ >= COMMIT_INTERVAL) {
        commit_transaction();
    }
}

ErrorStats ErrorLogger::get_stats() const {
    ErrorStats stats{};
    stats.postgis_read_errors = postgis_read_errors_.load();
    stats.postgis_parse_errors = postgis_parse_errors_.load();
    stats.mongo_write_errors = mongo_write_errors_.load();
    stats.mongo_flush_errors = mongo_flush_errors_.load();
    stats.mongo_connect_errors = mongo_connect_errors_.load();
    stats.general_errors = general_errors_.load();
    stats.total_errors = stats.postgis_read_errors + stats.postgis_parse_errors +
                         stats.mongo_write_errors + stats.mongo_flush_errors +
                         stats.mongo_connect_errors + stats.general_errors;
    return stats;
}

void ErrorLogger::print_summary(bool quiet_mode) const {
    auto stats = get_stats();
    if (stats.total_errors == 0 && quiet_mode) return;

    fprintf(stderr, "\nError Summary:\n");
    if (stats.postgis_read_errors > 0)
        fprintf(stderr, "  PostGIS read errors:    %zu\n", stats.postgis_read_errors);
    if (stats.postgis_parse_errors > 0)
        fprintf(stderr, "  PostGIS parse errors:   %zu\n", stats.postgis_parse_errors);
    if (stats.mongo_write_errors > 0)
        fprintf(stderr, "  MongoDB write errors:   %zu\n", stats.mongo_write_errors);
    if (stats.mongo_flush_errors > 0)
        fprintf(stderr, "  MongoDB flush errors:   %zu\n", stats.mongo_flush_errors);
    if (stats.mongo_connect_errors > 0)
        fprintf(stderr, "  MongoDB connect errors: %zu\n", stats.mongo_connect_errors);
    if (stats.general_errors > 0)
        fprintf(stderr, "  General errors:         %zu\n", stats.general_errors);

    if (stats.total_errors > 0) {
        fprintf(stderr, "  Total errors:           %zu\n", stats.total_errors);
        fprintf(stderr, "  Error log: %s\n", db_path_.c_str());
    } else {
        fprintf(stderr, "  No errors recorded.\n");
    }
}

void ErrorLogger::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ != nullptr) {
        if (uncommitted_ > 0) {
            commit_transaction();
        }
        sqlite3_close(reinterpret_cast<sqlite3*>(db_));
        db_ = nullptr;
    }
}
