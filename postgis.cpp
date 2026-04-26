#include "postgis.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <libpq-fe.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <chrono>
#include "geometry.hpp"
#include "serial.hpp"
#include "geojson.hpp"
#include "wkb_parser.hpp"
#include "error_logger.hpp"

extern int quiet;

std::unordered_map<std::string, int> PostGISReader::srid_cache_;
std::mutex PostGISReader::srid_cache_mutex_;
static std::unordered_map<std::string, std::pair<long long, long long>> shard_range_cache;
static std::mutex shard_range_cache_mutex;
static std::unordered_map<std::string, bool> selected_columns_validation_cache;
static std::mutex selected_columns_validation_cache_mutex;
static std::unordered_map<std::string, std::vector<std::string>> table_columns_cache;
static std::mutex table_columns_cache_mutex;
static std::unordered_map<std::string, std::vector<std::string>> sql_columns_cache;
static std::mutex sql_columns_cache_mutex;

static bool should_emit_postgis_runtime_logs(const postgis_config &config) {
    return config.enable_progress_report && !quiet;
}

void PostGISReader::reset_runtime_caches() {
    {
        std::lock_guard<std::mutex> lock(srid_cache_mutex_);
        srid_cache_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(shard_range_cache_mutex);
        shard_range_cache.clear();
    }
    {
        std::lock_guard<std::mutex> lock(selected_columns_validation_cache_mutex);
        selected_columns_validation_cache.clear();
    }
    {
        std::lock_guard<std::mutex> lock(table_columns_cache_mutex);
        table_columns_cache.clear();
    }
    {
        std::lock_guard<std::mutex> lock(sql_columns_cache_mutex);
        sql_columns_cache.clear();
    }
}

static std::string apply_shard_condition_to_query(const std::string &query, const std::string &shard_cond) {
    return PostGISReader::build_sharded_query(query, shard_cond);
}

static bool probe_query_ok(PGconn *conn, const std::string &query) {
    std::string probe_query = "SELECT 1 FROM (" + query + ") AS _probe LIMIT 1";
    PGresult *probe_res = PQexec(conn, probe_query.c_str());
    bool ok = (PQresultStatus(probe_res) == PGRES_TUPLES_OK);
    PQclear(probe_res);
    return ok;
}

static bool is_safe_identifier_token(const std::string &token) {
    if (token.empty()) {
        return false;
    }
    for (size_t i = 0; i < token.size(); i++) {
        char c = token[i];
        bool ok = (c == '_') || (c == '.') ||
                  (c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z');
        if (!ok) {
            return false;
        }
    }
    return true;
}

static std::string trim_copy(const std::string &s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) {
        b++;
    }
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) {
        e--;
    }
    return s.substr(b, e - b);
}

static bool looks_like_pg_array_literal(const std::string &s) {
    return s.size() >= 2 && s.front() == '{' && s.back() == '}' &&
           s.find(':') == std::string::npos && s.find('"') == std::string::npos;
}

static std::string minify_json_like(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    bool in_string = false;
    bool escape = false;
    for (char c : s) {
        if (escape) {
            out.push_back(c);
            escape = false;
            continue;
        }
        if (c == '\\') {
            out.push_back(c);
            if (in_string) {
                escape = true;
            }
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            out.push_back(c);
            continue;
        }
        if (!in_string && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
            continue;
        }
        out.push_back(c);
    }
    return out;
}

static std::string canonicalize_complex_text_attr(const char *value) {
    std::string s = trim_copy(value ? value : "");
    if (s.empty()) {
        return s;
    }

    // Match jsonb/to_json style for PostgreSQL array literals.
    if (looks_like_pg_array_literal(s)) {
        s.front() = '[';
        s.back() = ']';
        return minify_json_like(s);
    }

    // Match compact JSON object/array string style.
    if ((s.front() == '{' && s.back() == '}') || (s.front() == '[' && s.back() == ']')) {
        return minify_json_like(s);
    }

    return s;
}

static bool is_json_number_text(const std::string &s) {
    if (s.empty()) {
        return false;
    }
    size_t i = 0;
    if (s[i] == '-') {
        i++;
    }
    if (i >= s.size()) {
        return false;
    }
    if (s[i] == '0') {
        i++;
    } else {
        if (s[i] < '1' || s[i] > '9') {
            return false;
        }
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            i++;
        }
    }
    if (i < s.size() && s[i] == '.') {
        i++;
        if (i >= s.size() || s[i] < '0' || s[i] > '9') {
            return false;
        }
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            i++;
        }
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            i++;
        }
        if (i >= s.size() || s[i] < '0' || s[i] > '9') {
            return false;
        }
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            i++;
        }
    }
    return i == s.size();
}

static std::string unquote_json_string(const std::string &s) {
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') {
        return s;
    }
    std::string out;
    out.reserve(s.size() - 2);
    bool esc = false;
    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto parse_u16 = [&](size_t pos, uint16_t &code) -> bool {
        if (pos + 4 > s.size() - 1) {
            return false;
        }
        int v0 = hex_value(s[pos]);
        int v1 = hex_value(s[pos + 1]);
        int v2 = hex_value(s[pos + 2]);
        int v3 = hex_value(s[pos + 3]);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) {
            return false;
        }
        code = static_cast<uint16_t>((v0 << 12) | (v1 << 8) | (v2 << 4) | v3);
        return true;
    };
    auto append_utf8 = [&](uint32_t cp) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };
    for (size_t i = 1; i + 1 < s.size(); i++) {
        char c = s[i];
        if (esc) {
            switch (c) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(c);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    uint16_t u1 = 0;
                    if (!parse_u16(i + 1, u1)) {
                        out += "\\u";
                        break;
                    }
                    i += 4;
                    uint32_t cp = u1;
                    if (u1 >= 0xD800 && u1 <= 0xDBFF) {
                        if (i + 6 < s.size() - 1 && s[i + 1] == '\\' && s[i + 2] == 'u') {
                            uint16_t u2 = 0;
                            if (parse_u16(i + 3, u2) && u2 >= 0xDC00 && u2 <= 0xDFFF) {
                                cp = 0x10000 + (((uint32_t)(u1 - 0xD800) << 10) | (uint32_t)(u2 - 0xDC00));
                                i += 6;
                            }
                        }
                    }
                    append_utf8(cp);
                    break;
                }
                default:
                    out.push_back(c);
                    break;
            }
            esc = false;
            continue;
        }
        if (c == '\\') {
            esc = true;
            continue;
        }
        out.push_back(c);
    }
    return out;
}

static std::string escape_identifier(PGconn *pgconn, const std::string &identifier) {
    char *esc = PQescapeIdentifier(pgconn, identifier.c_str(), identifier.size());
    if (esc == NULL) {
        return "";
    }
    std::string out(esc);
    PQfreemem(esc);
    return out;
}

static std::string escape_literal(PGconn *pgconn, const std::string &value) {
    char *esc = PQescapeLiteral(pgconn, value.c_str(), value.size());
    if (esc == NULL) {
        return "";
    }
    std::string out(esc);
    PQfreemem(esc);
    return out;
}

static bool fetch_sql_subquery_columns(PGconn *pgconn, const postgis_config &cfg, std::unordered_map<std::string, bool> &existing) {
    std::string cache_key = cfg.host + "|" + cfg.port + "|" + cfg.dbname + "|" + cfg.sql;
    {
        std::lock_guard<std::mutex> lock(sql_columns_cache_mutex);
        auto it = sql_columns_cache.find(cache_key);
        if (it != sql_columns_cache.end()) {
            for (const std::string &c : it->second) {
                existing[c] = true;
            }
            return true;
        }
    }

    std::string probe_query = "SELECT * FROM (" + cfg.sql + ") AS _subq LIMIT 0";
    PGresult *res = PQexec(pgconn, probe_query.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        return false;
    }
    std::vector<std::string> cols;
    int nfields = PQnfields(res);
    for (int i = 0; i < nfields; i++) {
        std::string c = PQfname(res, i);
        existing[c] = true;
        cols.push_back(c);
    }
    PQclear(res);
    std::lock_guard<std::mutex> lock(sql_columns_cache_mutex);
    sql_columns_cache[cache_key] = cols;
    return true;
}

static bool fetch_sql_subquery_columns_ordered(PGconn *pgconn, const postgis_config &cfg, std::vector<std::string> &cols) {
    std::string cache_key = cfg.host + "|" + cfg.port + "|" + cfg.dbname + "|" + cfg.sql;
    {
        std::lock_guard<std::mutex> lock(sql_columns_cache_mutex);
        auto it = sql_columns_cache.find(cache_key);
        if (it != sql_columns_cache.end()) {
            cols = it->second;
            return true;
        }
    }

    std::string probe_query = "SELECT * FROM (" + cfg.sql + ") AS _subq LIMIT 0";
    PGresult *res = PQexec(pgconn, probe_query.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        return false;
    }
    int nfields = PQnfields(res);
    for (int i = 0; i < nfields; i++) {
        cols.push_back(PQfname(res, i));
    }
    PQclear(res);
    std::lock_guard<std::mutex> lock(sql_columns_cache_mutex);
    sql_columns_cache[cache_key] = cols;
    return true;
}

static bool fetch_table_columns(PGconn *pgconn, const postgis_config &cfg, const std::string &table, std::vector<std::string> &cols) {
    std::string cache_key = cfg.host + "|" + cfg.port + "|" + cfg.dbname + "|" + table;
    {
        std::lock_guard<std::mutex> lock(table_columns_cache_mutex);
        auto it = table_columns_cache.find(cache_key);
        if (it != table_columns_cache.end()) {
            cols = it->second;
            return true;
        }
    }

    std::string esc_table_literal = escape_literal(pgconn, table);
    if (esc_table_literal.empty()) {
        return false;
    }
    std::string query =
        "SELECT column_name FROM information_schema.columns "
        "WHERE table_schema = current_schema() AND table_name = " + esc_table_literal +
        " ORDER BY ordinal_position";
    PGresult *res = PQexec(pgconn, query.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        return false;
    }
    for (int i = 0; i < PQntuples(res); i++) {
        cols.push_back(PQgetvalue(res, i, 0));
    }
    PQclear(res);
    std::lock_guard<std::mutex> lock(table_columns_cache_mutex);
    table_columns_cache[cache_key] = cols;
    return true;
}

static std::string resolve_sql_geometry_field(PGconn *pgconn, const postgis_config &cfg) {
    std::unordered_map<std::string, bool> existing;
    if (!fetch_sql_subquery_columns(pgconn, cfg, existing)) {
        return "";
    }

    bool has_geom = existing.find("geom") != existing.end();
    bool has_geometry = existing.find("geometry") != existing.end();

    if (existing.find(cfg.geometry_field) != existing.end()) {
        return cfg.geometry_field;
    }

    // Only fall back automatically when there is a single obvious geometry candidate.
    if (cfg.geometry_field != "geometry" && !cfg.geometry_field.empty()) {
        fprintf(stderr, "Error: geometry field '%s' was not found in --postgis-sql result set\n",
                cfg.geometry_field.c_str());
        return "";
    }

    if (has_geom && !has_geometry) {
        return "geom";
    }
    if (has_geometry && !has_geom) {
        return "geometry";
    }

    if (has_geom && has_geometry) {
        fprintf(stderr, "Error: --postgis-sql result set contains both 'geom' and 'geometry'. Use --postgis-geometry-field explicitly.\n");
    }

    return "";
}

static bool validate_selected_columns(PGconn *pgconn, const postgis_config &cfg, const std::vector<std::string> &cols) {
    std::string cache_key = cfg.host + "|" + cfg.port + "|" + cfg.dbname + "|" + cfg.table + "|" +
                            cfg.geometry_field + "|" + cfg.sql + "|" + cfg.selected_columns_csv +
                            "|" + (cfg.selected_columns_best_effort ? "1" : "0");
    {
        std::lock_guard<std::mutex> lock(selected_columns_validation_cache_mutex);
        auto it = selected_columns_validation_cache.find(cache_key);
        if (it != selected_columns_validation_cache.end()) {
            return it->second;
        }
    }

    if (cols.empty()) {
        std::lock_guard<std::mutex> lock(selected_columns_validation_cache_mutex);
        selected_columns_validation_cache[cache_key] = true;
        return true;
    }
    for (size_t i = 0; i < cols.size(); i++) {
        if (!is_safe_identifier_token(cols[i])) {
            if (cfg.selected_columns_best_effort) {
                fprintf(stderr, "Warning: invalid column token '%s' skipped due to --postgis-columns-best-effort\n", cols[i].c_str());
                continue;
            }
            fprintf(stderr, "Error: invalid column token '%s' in --postgis-columns\n", cols[i].c_str());
            std::lock_guard<std::mutex> lock(selected_columns_validation_cache_mutex);
            selected_columns_validation_cache[cache_key] = false;
            return false;
        }
    }
    if (!cfg.has_input_source()) {
        std::lock_guard<std::mutex> lock(selected_columns_validation_cache_mutex);
        selected_columns_validation_cache[cache_key] = true;
        return true;
    }

    std::unordered_map<std::string, bool> existing;
    if (cfg.has_sql_input()) {
        if (!fetch_sql_subquery_columns(pgconn, cfg, existing)) {
            fprintf(stderr, "Error: failed to inspect columns for --postgis-sql: %s\n", PQerrorMessage(pgconn));
            std::lock_guard<std::mutex> lock(selected_columns_validation_cache_mutex);
            selected_columns_validation_cache[cache_key] = false;
            return false;
        }
    } else {
        std::vector<std::string> table_cols;
        if (!fetch_table_columns(pgconn, cfg, cfg.table, table_cols)) {
            fprintf(stderr, "Error: failed to inspect columns for table '%s': %s\n", cfg.table.c_str(), PQerrorMessage(pgconn));
            std::lock_guard<std::mutex> lock(selected_columns_validation_cache_mutex);
            selected_columns_validation_cache[cache_key] = false;
            return false;
        }
        for (const std::string &col : table_cols) {
            existing[col] = true;
        }
    }

    bool ok = true;
    for (size_t i = 0; i < cols.size(); i++) {
        const std::string &c = cols[i];
        if (c == "wkb" || c == cfg.geometry_field) {
            continue;
        }
        if (existing.find(c) == existing.end()) {
            if (cfg.selected_columns_best_effort) {
                fprintf(stderr, "Warning: unknown column '%s' skipped due to --postgis-columns-best-effort\n", c.c_str());
            } else {
                fprintf(stderr, "Error: column '%s' from --postgis-columns does not exist in source columns\n", c.c_str());
                ok = false;
            }
        }
    }
    std::lock_guard<std::mutex> lock(selected_columns_validation_cache_mutex);
    selected_columns_validation_cache[cache_key] = ok;
    return ok;
}

static std::string build_json_projection(PGconn *pgconn,
                                         const postgis_config &cfg,
                                         const std::vector<std::string> &source_cols,
                                         const std::unordered_map<std::string, bool> &existing_cols) {
    std::string projected;
    if (!cfg.selected_columns_csv.empty()) {
        std::vector<std::string> cols = split_csv_list(cfg.selected_columns_csv);
        for (const std::string &col : cols) {
            if (col == "wkb" || col == cfg.geometry_field) {
                continue;
            }
            if (!is_safe_identifier_token(col)) {
                if (cfg.selected_columns_best_effort) {
                    continue;
                }
                return "";
            }
            std::string esc_col = escape_identifier(pgconn, col);
            if (esc_col.empty()) {
                if (cfg.selected_columns_best_effort) {
                    continue;
                }
                return "";
            }
            if (cfg.selected_columns_best_effort && !existing_cols.empty() && existing_cols.find(col) == existing_cols.end()) {
                continue;
            }
            if (!projected.empty()) {
                projected += ", ";
            }
            projected += "to_jsonb(" + esc_col + ")::text AS " + esc_col;
        }
        return projected;
    }

    for (const std::string &col : source_cols) {
        if (col == "wkb" || col == cfg.geometry_field) {
            continue;
        }
        std::string esc_col = escape_identifier(pgconn, col);
        if (esc_col.empty()) {
            continue;
        }
        if (!projected.empty()) {
            projected += ", ";
        }
        projected += "to_jsonb(" + esc_col + ")::text AS " + esc_col;
    }
    return projected;
}

struct ShardPlan {
    bool applied = false;
    bool no_work = false;
    bool fatal = false;
    std::string query;
};

static ShardPlan prepare_shard_plan(PGconn *pgconn,
                                    const postgis_config &config,
                                    const std::string &base_query,
                                    size_t thread_id,
                                    size_t num_threads) {
    ShardPlan plan;
    plan.query = base_query;
    std::string shard_mode = config.effective_shard_mode();

    if (config.is_none_shard_mode()) {
        if (thread_id == 0) {
            return plan;
        }
        plan.no_work = true;
        plan.applied = true;
        return plan;
    }

    if (!config.shard_key.empty()) {
        char *esc_shard_key = PQescapeIdentifier(pgconn, config.shard_key.c_str(), config.shard_key.size());
        if (esc_shard_key == NULL) {
            if (config.is_key_shard_mode() || config.is_range_shard_mode()) {
                fprintf(stderr, "Thread %zu: failed to escape shard key '%s'\n", thread_id, config.shard_key.c_str());
                plan.fatal = true;
            }
            return plan;
        }
        std::string esc_key = std::string(esc_shard_key);

        if (config.can_attempt_range_sharding()) {
            std::string range_cache_key = config.host + "|" + config.port + "|" + config.dbname + "|" + config.shard_key + "|" + base_query;
            long long min_key = 0;
            long long max_key = -1;
            bool has_range = false;
            {
                std::lock_guard<std::mutex> lock(shard_range_cache_mutex);
                auto it = shard_range_cache.find(range_cache_key);
                if (it != shard_range_cache.end()) {
                    min_key = it->second.first;
                    max_key = it->second.second;
                    has_range = true;
                }
            }

            if (!has_range) {
                std::string range_query =
                    "SELECT MIN(_k), MAX(_k) FROM ("
                    "SELECT CAST(" + esc_key + " AS bigint) AS _k FROM (" + base_query + ") AS _range_base "
                    "WHERE " + esc_key + " IS NOT NULL) AS _range_mm";
                PGresult *range_res = PQexec(pgconn, range_query.c_str());
                if (PQresultStatus(range_res) == PGRES_TUPLES_OK &&
                    PQntuples(range_res) > 0 &&
                    !PQgetisnull(range_res, 0, 0) &&
                    !PQgetisnull(range_res, 0, 1)) {
                    min_key = strtoll(PQgetvalue(range_res, 0, 0), NULL, 10);
                    max_key = strtoll(PQgetvalue(range_res, 0, 1), NULL, 10);
                    has_range = true;
                    std::lock_guard<std::mutex> lock(shard_range_cache_mutex);
                    shard_range_cache[range_cache_key] = std::make_pair(min_key, max_key);
                } else if (config.is_range_shard_mode()) {
                    const char *db_err = PQerrorMessage(pgconn);
                    if (db_err != NULL && strstr(db_err, "invalid input syntax for type bigint") != NULL) {
                        fprintf(stderr, "Thread %zu: range sharding key '%s' is non-numeric; use --postgis-shard-mode=key or auto\n",
                                thread_id, config.shard_key.c_str());
                    } else {
                        fprintf(stderr, "Thread %zu: range sharding metadata query failed for key '%s': %s\n",
                                thread_id, config.shard_key.c_str(), db_err ? db_err : "unknown error");
                    }
                }
                PQclear(range_res);
            }

            if (has_range && max_key >= min_key) {
                    unsigned long long span = static_cast<unsigned long long>(max_key - min_key) + 1ULL;
                    unsigned long long chunk = (span + num_threads - 1) / num_threads;
                    if (thread_id > 0 && chunk > ULLONG_MAX / thread_id) {
                         DEBUG_LOG("Thread %zu: range shard overflow detected, skipping", thread_id);
                    } else {
                        unsigned long long offset = chunk * thread_id;
                        long long start = min_key + static_cast<long long>(offset);
                        long long end = (thread_id + 1 >= num_threads) ? (max_key + 1) : (min_key + static_cast<long long>(offset + chunk));
                        if (start < end) {
                            std::string range_cond = "(" + esc_key + " >= " + std::to_string(start) +
                                                     " AND " + esc_key + " < " + std::to_string(end) + ")";
                            std::string range_sharded_query = apply_shard_condition_to_query(base_query, range_cond);
                            if (probe_query_ok(pgconn, range_sharded_query)) {
                                plan.query = range_sharded_query;
                                plan.applied = true;
                                DEBUG_LOG("Thread %zu/%zu: using range sharding on key %s [%lld, %lld)", thread_id, num_threads, config.shard_key.c_str(), start, end);
                            } else if (config.is_range_shard_mode()) {
                                fprintf(stderr, "Thread %zu: range sharding probe failed for key '%s'\n", thread_id, config.shard_key.c_str());
                            }
                        } else {
                            plan.applied = true;
                            plan.no_work = true;
                            DEBUG_LOG("Thread %zu/%zu: no range-shard work for key %s", thread_id, num_threads, config.shard_key.c_str());
                        }
                    }
            }

            if (!plan.applied && config.is_range_shard_mode()) {
                fprintf(stderr, "Thread %zu: configured range shard key '%s' is not usable; try shard-mode=key or auto\n", thread_id, config.shard_key.c_str());
                PQfreemem(esc_shard_key);
                plan.fatal = true;
                return plan;
            }
        }

        if (!plan.no_work && !plan.applied && config.can_attempt_key_sharding()) {
            std::string key_cond =
                "(abs(hashtext(COALESCE(CAST(" + esc_key + " AS text), ''))) % " + std::to_string(num_threads) + ") = " + std::to_string(thread_id);
            std::string key_sharded_query = apply_shard_condition_to_query(base_query, key_cond);
            if (probe_query_ok(pgconn, key_sharded_query)) {
                plan.query = key_sharded_query;
                plan.applied = true;
                DEBUG_LOG("Thread %zu/%zu: using hash-based sharding on key %s", thread_id, num_threads, config.shard_key.c_str());
            } else if (config.is_key_shard_mode()) {
                fprintf(stderr, "Thread %zu: configured shard key '%s' is not usable for key-hash sharding\n", thread_id, config.shard_key.c_str());
                PQfreemem(esc_shard_key);
                plan.fatal = true;
                return plan;
            }
        }

        PQfreemem(esc_shard_key);
    }

    if (!plan.no_work && !plan.applied && config.can_attempt_ctid_sharding()) {
        std::string ctid_cond = "(abs(hashtext(ctid::text)) % " + std::to_string(num_threads) + ") = " + std::to_string(thread_id);
        std::string ctid_sharded_query = apply_shard_condition_to_query(base_query, ctid_cond);
        if (probe_query_ok(pgconn, ctid_sharded_query)) {
            plan.query = ctid_sharded_query;
            plan.applied = true;
            DEBUG_LOG("Thread %zu/%zu: using hash-based sharding on ctid", thread_id, num_threads);
        }
    }

    return plan;
}

PostGISReader::PostGISReader(const postgis_config &cfg) : config(cfg), conn(NULL)
{
    config.normalize();
}

PostGISReader::~PostGISReader()
{
    if (conn)
    {
        PQfinish((PGconn *)conn);
    }
}

bool PostGISReader::connect()
{
    const char *keywords[] = {"host", "port", "dbname", "user", "password", "connect_timeout", NULL};
    std::string timeout_str = std::to_string(POSTGIS_CONNECTION_TIMEOUT_SEC);
    const char *values[] = {
        config.host.c_str(),
        config.port.c_str(),
        config.dbname.c_str(),
        config.user.c_str(),
        config.password.c_str(),
        timeout_str.c_str(),
        NULL};

    DEBUG_LOG("Connecting to PostGIS (%s:%s/%s)...", config.host.c_str(), config.port.c_str(), config.dbname.c_str());
    conn = PQconnectdbParams(keywords, values, 0);

    if (PQstatus((PGconn *)conn) != CONNECTION_OK)
    {
        fprintf(stderr, "Error: Connection to PostGIS failed: %s\n", PQerrorMessage((PGconn *)conn));
        PQfinish((PGconn *)conn);
        conn = NULL;
        return false;
    }

    DEBUG_LOG("Connected to PostGIS successfully.");
    return true;
}

void PostGISReader::disconnect()
{
    if (conn)
    {
        PQfinish((PGconn *)conn);
        conn = NULL;
    }
}

int PostGISReader::get_cached_srid(const postgis_config &cfg, void *conn_ptr)
{
    if (!conn_ptr)
    {
        return 0;
    }

    PGconn *pgconn = static_cast<PGconn *>(conn_ptr);
    std::string cache_key = cfg.host + "|" + cfg.port + "|" + cfg.dbname + "|" + cfg.table + "|" + cfg.geometry_field + "|" + cfg.sql;

    {
        std::lock_guard<std::mutex> lock(srid_cache_mutex_);
        auto it = srid_cache_.find(cache_key);
        if (it != srid_cache_.end())
        {
            return it->second;
        }
    }

    std::string srid_query;
    int resolved_srid = 0;

    if (cfg.has_sql_input())
    {
        std::string sql_geom_field = resolve_sql_geometry_field(pgconn, cfg);
        if (sql_geom_field.empty()) {
            return resolved_srid;
        }
        std::string esc_geom = escape_identifier(pgconn, sql_geom_field);
        if (esc_geom.empty()) {
            return resolved_srid;
        }
        srid_query = "SELECT ST_SRID(" + esc_geom + ") FROM (" + cfg.sql + ") AS _subq LIMIT 1";
    }
    else if (cfg.has_table_input() && !cfg.geometry_field.empty())
    {
        char *esc_geom = PQescapeIdentifier(pgconn, cfg.geometry_field.c_str(), cfg.geometry_field.size());
        char *esc_table = PQescapeIdentifier(pgconn, cfg.table.c_str(), cfg.table.size());

        if (!esc_geom || !esc_table)
        {
            fprintf(stderr, "Error: Failed to escape SQL identifiers: %s\n", PQerrorMessage(pgconn));
            PQfreemem(esc_geom);
            PQfreemem(esc_table);
            return resolved_srid;
        }

        srid_query = "SELECT ST_SRID(" + std::string(esc_geom) + ") FROM " + std::string(esc_table) + " LIMIT 1";
        PQfreemem(esc_geom);
        PQfreemem(esc_table);
    }
    else
    {
        return resolved_srid;
    }

    PGresult *res = PQexec(pgconn, srid_query.c_str());
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0 && !PQgetisnull(res, 0, 0))
    {
        const char *srid_str = PQgetvalue(res, 0, 0);
        char *end_ptr = nullptr;
        long srid_val = strtol(srid_str, &end_ptr, 10);
        if (end_ptr != srid_str && *end_ptr == '\0' && srid_val >= 0 && srid_val <= INT_MAX) {
            resolved_srid = static_cast<int>(srid_val);
        } else {
            fprintf(stderr, "Warning: Invalid SRID value '%s', defaulting to 0\n", srid_str);
        }
    }
    PQclear(res);

    {
        std::lock_guard<std::mutex> lock(srid_cache_mutex_);
        srid_cache_[cache_key] = resolved_srid;
    }

    DEBUG_LOG("Cached geometry SRID = %d for key %s", resolved_srid, cache_key.c_str());
    return resolved_srid;
}

std::string PostGISReader::build_select_query(const postgis_config &cfg, int srid, void *conn_ptr)
{
    PGconn *pgconn = static_cast<PGconn *>(conn_ptr);
    std::vector<std::string> source_cols;
    std::unordered_map<std::string, bool> existing_cols;
    if (cfg.has_sql_input()) {
        if (!fetch_sql_subquery_columns_ordered(pgconn, cfg, source_cols)) {
            return "";
        }
        for (const std::string &col : source_cols) {
            existing_cols[col] = true;
        }
    } else if (cfg.has_table_input()) {
        if (!fetch_table_columns(pgconn, cfg, cfg.table, source_cols)) {
            return "";
        }
        for (const std::string &col : source_cols) {
            existing_cols[col] = true;
        }
    }

    std::string projection = build_json_projection(pgconn, cfg, source_cols, existing_cols);
    if (projection.empty() && !cfg.selected_columns_csv.empty() && !cfg.selected_columns_best_effort) {
        return "";
    }
    if (projection.empty()) {
        projection = "*";
    }

    if (cfg.has_sql_input())
    {
        std::string sql_geom_field = resolve_sql_geometry_field(pgconn, cfg);
        if (sql_geom_field.empty()) {
            fprintf(stderr, "Error: cannot resolve geometry field from --postgis-sql result set. Use --postgis-geometry-field.\n");
            return "";
        }
        std::string esc_geom = escape_identifier(pgconn, sql_geom_field);
        if (esc_geom.empty()) {
            return "";
        }
        if (srid == 4326)
        {
            return "SELECT ST_AsBinary(" + esc_geom + ") AS wkb, " + projection + " FROM (" + cfg.sql + ") AS _subq";
        }
        else
        {
            return "SELECT ST_AsBinary(ST_Transform(" + esc_geom + ", 4326)) AS wkb, " + projection + " FROM (" + cfg.sql + ") AS _subq";
        }
    }

    std::string esc_geom_str = escape_identifier(pgconn, cfg.geometry_field);
    std::string esc_table_str = escape_identifier(pgconn, cfg.table);
    if (esc_geom_str.empty() || esc_table_str.empty()) {
        return "";
    }

    if (srid == 4326)
    {
        return "SELECT ST_AsBinary(" + esc_geom_str + ") AS wkb, " + projection + " FROM " + esc_table_str;
    }
    else
    {
        return "SELECT ST_AsBinary(ST_Transform(" + esc_geom_str + ", 4326)) AS wkb, " + projection + " FROM " + esc_table_str;
    }
}

bool PostGISReader::check_memory_usage()
{
    size_t estimated = current_memory_usage.load();
    size_t limit = config.max_memory_mb * 1024 * 1024;

    if (estimated > limit)
    {
        fprintf(stderr, "ERROR: Memory usage (%zu MB) exceeds limit (%zu MB)\n",
                estimated / (1024 * 1024), config.max_memory_mb);
        return false;
    }

    if (estimated > limit * 0.8 && should_emit_postgis_runtime_logs(config))
    {
        fprintf(stderr, "WARNING: Memory usage at %.1f%% (%zu MB / %zu MB)\n",
                (double)estimated / limit * 100.0,
                estimated / (1024 * 1024),
                config.max_memory_mb);
    }

    return true;
}

void PostGISReader::log_progress(size_t processed, size_t total, const char *stage)
{
    if (!should_emit_postgis_runtime_logs(config))
        return;

    if (total > 0)
    {
        double percent = (processed * 100.0) / total;
        fprintf(stderr, "Progress: %s - %zu/%zu (%.1f%%)\n", stage, processed, total, percent);
    }
    else
    {
        fprintf(stderr, "Progress: %s - %zu features processed\n", stage, processed);
    }
}

std::string PostGISReader::escape_json_string(const char *value)
{
    if (!value)
        return "";

    std::string result;
    const size_t len = strlen(value);
    result.reserve(len * 2);

    for (size_t k = 0; k < len; k++)
    {
        char c = value[k];
        switch (c)
        {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                result += buf;
            }
            else
            {
                result += c;
            }
            break;
        }
    }

    return result;
}

std::vector<uint8_t> PostGISReader::decode_bytea(const char *hex_data, size_t hex_len)
{
    std::vector<uint8_t> result;

    if (hex_len >= 2 && hex_data[0] == '\\' && hex_data[1] == 'x')
    {
        hex_data += 2;
        hex_len -= 2;
    }

    if (hex_len % 2 != 0)
    {
        return result;
    }

    result.reserve(hex_len / 2);

    for (size_t i = 0; i < hex_len; i += 2)
    {
        uint8_t hi = 0, lo = 0;
        char ch = hex_data[i];
        if (ch >= '0' && ch <= '9') hi = ch - '0';
        else if (ch >= 'a' && ch <= 'f') hi = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') hi = ch - 'A' + 10;
        else return std::vector<uint8_t>();

        ch = hex_data[i + 1];
        if (ch >= '0' && ch <= '9') lo = ch - '0';
        else if (ch >= 'a' && ch <= 'f') lo = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') lo = ch - 'A' + 10;
        else return std::vector<uint8_t>();

        result.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }

    return result;
}

void PostGISReader::process_feature(PGresult *res, int row, int nfields, int wkb_field_index,
                                    const std::vector<std::string> &field_names,
                                    std::vector<struct serialization_state> &sst, size_t layer,
                                    const std::string &layername, size_t thread_id)
{
    if (PQgetisnull(res, row, wkb_field_index))
    {
        parse_errors_.fetch_add(1);
        ErrorLogger::instance().log_parse_error(row + 1, "NULL", "NULL WKB geometry", "");
        return;
    }

    char *wkb_hex = PQgetvalue(res, row, wkb_field_index);
    int wkb_len = PQgetlength(res, row, wkb_field_index);

    if (wkb_hex == NULL || wkb_len == 0)
    {
        parse_errors_.fetch_add(1);
        ErrorLogger::instance().log_parse_error(row + 1, "EMPTY", "Empty WKB data", "");
        return;
    }

    std::vector<uint8_t> wkb_bytes = decode_bytea(wkb_hex, wkb_len);
    if (wkb_bytes.empty())
    {
        parse_errors_.fetch_add(1);
        std::string preview(wkb_hex, std::min(wkb_len, 60));
        ErrorLogger::instance().log_parse_error(row + 1, "BYTEA", "Failed to decode bytea data", preview);
        return;
    }

    WKBResult result = parse_wkb(wkb_bytes.data(), wkb_bytes.size());
    if (!result.valid)
    {
        parse_errors_.fetch_add(1);
        std::string preview(wkb_hex, std::min(wkb_len, 60));
        ErrorLogger::instance().log_parse_error(row + 1, "WKB_PARSE", result.error, preview);
        return;
    }

    size_t coord_memory = result.coordinates.capacity() * sizeof(draw);
    current_memory_usage.fetch_add(coord_memory);

    if (result.geometry_type < 0 || result.geometry_type > GEOM_GEOMETRYCOLLECTION)
    {
        parse_errors_.fetch_add(1);
        ErrorLogger::instance().log_parse_error(row + 1, "UNSUPPORTED",
            "Unsupported geometry type: " + std::to_string(result.geometry_type), "");
        current_memory_usage.fetch_sub(coord_memory);
        return;
    }

    serial_feature sf;
    sf.layer = layer;
    sf.segment = thread_id % sst.size();
    int geom_idx = result.geometry_type;
    if (geom_idx == GEOM_GEOMETRYCOLLECTION) {
        geom_idx = GEOM_MULTIPOLYGON;
    }
    sf.t = mb_geometry[geom_idx];
    sf.geometry = result.coordinates;
    sf.has_id = false;
    sf.id = 0;
    sf.tippecanoe_minzoom = -1;
    sf.tippecanoe_maxzoom = -1;
    sf.feature_minzoom = 0;
    sf.seq = *(sst[thread_id % sst.size()].layer_seq);

    struct AttrKV {
        std::string key;
        serial_val value;
    };
    std::vector<AttrKV> attrs;

    size_t attr_memory = 0;

    for (int field = 0; field < nfields; field++)
    {
        if (field == wkb_field_index)
            continue;

        const std::string &fieldname = field_names[field];
        if (fieldname.empty() || fieldname == config.geometry_field || fieldname == "wkb")
        {
            continue;
        }

        if (PQgetisnull(res, row, field))
            continue;

        char *value = PQgetvalue(res, row, field);
        if (value == NULL)
            continue;

        serial_val sv;
        std::string raw = value;
        if (raw == "null") {
            continue;
        } else if (raw == "true" || raw == "false") {
            sv.type = mvt_bool;
            sv.s = raw;
        } else if (is_json_number_text(raw)) {
            sv.type = mvt_double;
            sv.s = raw;
        } else if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
            sv.type = mvt_string;
            sv.s = unquote_json_string(raw);
        } else {
            sv.type = mvt_string;
            sv.s = canonicalize_complex_text_attr(raw.c_str());
        }
        attrs.push_back({fieldname, sv});
        attr_memory += fieldname.size() + strlen(value) + sizeof(std::string) + 32;
    }

    if (config.canonical_attr_order) {
        static const std::unordered_map<std::string, int> preferred_attr_order = {
            {"id", 0},
            {"name", 1},
            {"level", 2},
            {"adchar", 3},
            {"adcode", 4},
            {"center", 5},
            {"parent", 6},
            {"acroutes", 7},
            {"centroid", 8},
            {"childrennum", 9},
            {"subfeatureindex", 10},
        };
        std::sort(attrs.begin(), attrs.end(), [](const AttrKV &a, const AttrKV &b) {
            auto ia = preferred_attr_order.find(a.key);
            auto ib = preferred_attr_order.find(b.key);
            int ra = (ia == preferred_attr_order.end()) ? 1000000 : ia->second;
            int rb = (ib == preferred_attr_order.end()) ? 1000000 : ib->second;
            if (ra != rb) {
                return ra < rb;
            }
            return a.key < b.key;
        });
    } else {
        // Preserve the SELECT/result-set field order so PostGIS input can match
        // native tippecanoe output when users disable canonical attribute sorting.
    }

    std::vector<std::shared_ptr<std::string>> full_keys;
    std::vector<serial_val> values;
    full_keys.reserve(attrs.size());
    values.reserve(attrs.size());
    for (auto &kv : attrs) {
        full_keys.emplace_back(std::make_shared<std::string>(kv.key));
        values.push_back(kv.value);
    }

    sf.full_keys = std::move(full_keys);
    sf.full_values = std::move(values);

    current_memory_usage.fetch_add(attr_memory);

    struct serialization_state temp_sst = sst[thread_id % sst.size()];
    temp_sst.fname = "PostGIS";
    temp_sst.line = row + 1;

    serialize_feature(&temp_sst, sf, layername);

    current_memory_usage.fetch_sub(coord_memory + attr_memory);

    total_features_processed.fetch_add(1);
}

void PostGISReader::process_batch(PGresult *res, std::vector<struct serialization_state> &sst,
                                  size_t layer, const std::string &layername, int wkb_field_index,
                                  size_t thread_id)
{
    int ntuples = PQntuples(res);
    int nfields = PQnfields(res);

    std::vector<std::string> field_names;
    field_names.reserve(nfields);
    for (int i = 0; i < nfields; i++)
    {
        field_names.push_back(PQfname(res, i));
    }

    for (int i = 0; i < ntuples; i++)
    {
        process_feature(res, i, nfields, wkb_field_index, field_names, sst, layer, layername, thread_id);

        if (i % 100 == 0 && !check_memory_usage())
        {
            fprintf(stderr, "Memory pressure at feature %d, pausing...\n", i);
            usleep(100000);
        }
    }

    total_batches_processed.fetch_add(1);
    log_progress(total_features_processed.load(), 0, "Processing features");
}

bool PostGISReader::execute_query_with_retry(const std::string &query)
{
    int retries = 0;
    while (retries < config.max_retries)
    {
        if (execute_query(query))
        {
            return true;
        }

        retries++;
        if (retries < config.max_retries)
        {
            if (should_emit_postgis_runtime_logs(config)) {
                fprintf(stderr, "Retrying query (attempt %d/%d)...\n", retries + 1, config.max_retries);
            }
            usleep(1000000);
        }
    }

    return false;
}

bool PostGISReader::execute_query(const std::string &query)
{
    if (!conn)
    {
        fprintf(stderr, "Error: Not connected to database\n");
        return false;
    }

    PGresult *res = PQexec((PGconn *)conn, query.c_str());
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK)
    {
        fprintf(stderr, "Query failed: %s\n", PQerrorMessage((PGconn *)conn));
        PQclear(res);
        return false;
    }

    PQclear(res);
    return true;
}

bool PostGISReader::read_features(std::vector<struct serialization_state> &sst, size_t layer, const std::string &layername,
                                  size_t thread_id, size_t num_threads)
{
    auto t_start = std::chrono::steady_clock::now();
    long long t_count_ms = 0;

    if (!conn)
    {
        if (!connect())
        {
            return false;
        }
    }

    int srid = get_cached_srid(config, conn);
    PGconn *pgconn = static_cast<PGconn *>(conn);
    std::vector<std::string> selected_cols = split_csv_list(config.selected_columns_csv);
    if (!validate_selected_columns(pgconn, config, selected_cols)) {
        return false;
    }

    std::string base_query;
    if (config.has_sql_input())
    {
        base_query = build_select_query(config, srid, conn);
    }
    else if (config.has_table_input() && !config.geometry_field.empty())
    {
        base_query = build_select_query(config, srid, conn);
    }
    else
    {
        fprintf(stderr, "Error: Either --postgis-sql or both --postgis-table and --postgis-geometry-field are required\n");
        return false;
    }
    if (base_query.empty()) {
        fprintf(stderr, "Error: Failed to build PostGIS select query. Please verify geometry/table/columns options.\n");
        return false;
    }

    if (num_threads > 1)
    {
        std::string shard_mode = config.effective_shard_mode();
        ShardPlan plan = prepare_shard_plan(pgconn, config, base_query, thread_id, num_threads);
        if (plan.fatal) {
            return false;
        }
        if (plan.no_work) {
            log_progress(0, 0, "Thread has no shard range");
            return true;
        }
        base_query = plan.query;

        if (!plan.applied) {
            if (thread_id == 0 && should_emit_postgis_runtime_logs(config)) {
                fprintf(stderr, "Thread %zu: sharding not available in mode '%s', falling back to sequential read\n", thread_id, shard_mode.c_str());
                if (shard_mode == "auto") {
                    fprintf(stderr, "  Diagnostic: auto mode failed because ctid is not available in subquery results\n");
                    fprintf(stderr, "  Solution: Use --postgis-shard-key=<column> --postgis-shard-mode=key\n");
                    fprintf(stderr, "  Example: --postgis-shard-key=ogc_fid --postgis-shard-mode=key\n");
                }
            }
            base_query = build_select_query(config, srid, conn);
            if (thread_id != 0) {
                log_progress(0, 0, "Non-primary thread exiting (no sharding support)");
                return true;
            }
        }
    }

    size_t total_count = 0;
    if (config.enable_progress_report && config.progress_with_exact_count)
    {
        auto t_count_begin = std::chrono::steady_clock::now();
        std::string count_query = "SELECT COUNT(*) FROM (" + base_query + ") AS _count_subq";
        DEBUG_LOG("Executing count query");
        PGresult *count_res = PQexec((PGconn *)conn, count_query.c_str());
        if (PQresultStatus(count_res) == PGRES_TUPLES_OK && PQntuples(count_res) > 0)
        {
            total_count = strtoull(PQgetvalue(count_res, 0, 0), NULL, 10);
        }
        else
        {
            DEBUG_LOG("Count query failed: %s", PQerrorMessage((PGconn *)conn));
        }
        PQclear(count_res);
        t_count_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t_count_begin)
                         .count();
    }

    auto find_wkb_field_index = [&](PGresult *res, const char *context) -> int {
        int nfields = PQnfields(res);
        for (int i = 0; i < nfields; i++) {
            if (strcmp(PQfname(res, i), "wkb") == 0) {
                return i;
            }
        }
        fprintf(stderr, "Thread %zu: WKB geometry field not found in %s result\n", thread_id, context);
        fprintf(stderr, "Available fields: ");
        for (int i = 0; i < nfields; i++) {
            fprintf(stderr, "%s ", PQfname(res, i));
        }
        fprintf(stderr, "\n");
        return -1;
    };

    auto run_standard_query_mode = [&]() -> bool {
        if (should_emit_postgis_runtime_logs(config)) {
            fprintf(stderr, "Thread %zu: Using standard query mode\n", thread_id);
        }
        PGresult *res = PQexec((PGconn *)conn, base_query.c_str());
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            fprintf(stderr, "Thread %zu: Query failed: %s\n", thread_id, PQerrorMessage((PGconn *)conn));
            PQclear(res);
            return false;
        }
        int ntuples = PQntuples(res);
        if (should_emit_postgis_runtime_logs(config)) {
            fprintf(stderr, "Thread %zu: Processing %d features\n", thread_id, ntuples);
        }
        int wkb_field_index = find_wkb_field_index(res, "query");
        if (wkb_field_index == -1) {
            PQclear(res);
            return false;
        }
        process_batch(res, sst, layer, layername, wkb_field_index, thread_id);
        PQclear(res);
        return true;
    };

    auto close_cursor_tx = [&](const std::string &cursor_name) {
        execute_query("CLOSE " + cursor_name);
        execute_query("COMMIT");
    };

    if (config.use_cursor && (total_count == 0 || total_count > config.batch_size))
    {
        if (should_emit_postgis_runtime_logs(config)) {
            fprintf(stderr, "Thread %zu: Using cursor-based batch processing (batch size: %zu)\n",
                    thread_id, config.batch_size);
        }

        std::string cursor_name = "pg_cursor_" + std::to_string(getpid()) + "_" + std::to_string(thread_id);
        std::string begin_query = "BEGIN ISOLATION LEVEL REPEATABLE READ";
        std::string declare_query = "DECLARE " + cursor_name + " SCROLL CURSOR FOR " + base_query;

        bool cursor_ok = false;
        if (execute_query(begin_query))
        {
            if (execute_query(declare_query))
            {
                cursor_ok = true;
            }
            else
            {
                if (should_emit_postgis_runtime_logs(config)) {
                    fprintf(stderr, "Thread %zu: Failed to declare cursor, falling back to non-cursor mode\n", thread_id);
                }
                execute_query("ROLLBACK");
            }
        }
        else
        {
            if (should_emit_postgis_runtime_logs(config)) {
                fprintf(stderr, "Thread %zu: Failed to begin transaction, falling back to non-cursor mode\n", thread_id);
            }
        }

        if (cursor_ok)
        {
            size_t offset = 0;
            while (true)
            {
                std::string fetch_query = "FETCH FORWARD " + std::to_string(config.batch_size) + " FROM " + cursor_name;
                PGresult *res = PQexec((PGconn *)conn, fetch_query.c_str());

                if (PQresultStatus(res) != PGRES_TUPLES_OK)
                {
                    fprintf(stderr, "Thread %zu: Fetch failed: %s\n", thread_id, PQerrorMessage((PGconn *)conn));
                    PQclear(res);
                    break;
                }

                int ntuples = PQntuples(res);
                if (ntuples == 0)
                {
                    PQclear(res);
                    break;
                }

                int wkb_field_index = find_wkb_field_index(res, "cursor");
                if (wkb_field_index == -1)
                {
                    PQclear(res);
                    close_cursor_tx(cursor_name);
                    return false;
                }

                process_batch(res, sst, layer, layername, wkb_field_index, thread_id);

                offset += ntuples;
                log_progress(offset, total_count, "Fetching batches");

                PQclear(res);

                if (!check_memory_usage())
                {
                    fprintf(stderr, "Thread %zu: Memory pressure detected, pausing batch fetch...\n", thread_id);
                    usleep(500000);
                }
            }

            close_cursor_tx(cursor_name);
        }
        else
        {
            if (!run_standard_query_mode()) {
                return false;
            }
        }
    }
    else
    {
        if (!run_standard_query_mode()) {
            return false;
        }
    }

    log_progress(total_features_processed.load(), total_count, "Completed");
    if (should_emit_postgis_runtime_logs(config)) {
        fprintf(stderr, "Thread %zu: Total features processed: %zu in %zu batches (parse errors: %zu)\n",
                thread_id, total_features_processed.load(), total_batches_processed.load(), parse_errors_.load());
    }
    if (config.profile && !quiet) {
        long long t_total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - t_start)
                                   .count();
        fprintf(stderr,
                "PostGIS profile (thread=%zu): total=%lld ms, count=%lld ms, features=%zu, batches=%zu\n",
                thread_id, t_total_ms, t_count_ms, total_features_processed.load(), total_batches_processed.load());
    }

    return true;
}
std::string PostGISReader::build_sharded_query(const std::string &base_query, const std::string &shard_condition) {
    if (base_query.empty() || shard_condition.empty()) {
        return base_query;
    }

    return "SELECT * FROM (" + base_query + ") AS _shard_src WHERE " + shard_condition;
}
