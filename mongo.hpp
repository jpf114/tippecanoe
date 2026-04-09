#ifndef MONGO_HPP
#define MONGO_HPP

#include <string>
#include <vector>
#include <tuple>
#include <atomic>
#include <memory>
#include <mutex>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/document/view.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>
#include <mongocxx/options/insert.hpp>
#include <mongocxx/options/replace.hpp>
#include <mongocxx/write_concern.hpp>
#include <mongocxx/options/client.hpp>
#include "config.hpp"

// MongoDB 写确认级别枚举
enum class WriteConcernLevel {
    NONE = 0,      // w:0 - 不等待确认，最快
    PRIMARY = 1,   // w:1 - 等待主节点确认
    MAJORITY = 2   // w:"majority" - 等待多数节点确认，最安全
};

// MongoDB 配置结构
struct mongo_config {
    // 连接参数
    std::string host;
    int port;
    std::string dbname;
    std::string collection;
    std::string username;
    std::string password;
    std::string auth_source;
    
    // 性能参数
    size_t batch_size;
    size_t connection_pool_size;
    int timeout_ms;
    int max_retries;
    
    // 写入参数
    WriteConcernLevel write_concern_level;
    bool journal;
    int wtimeout_ms;
    
    // 索引参数
    bool create_indexes;
    
    // 集合管理参数
    bool drop_collection_before_write;
    
    // 元数据参数
    bool write_metadata;
    
    // 监控参数
    bool enable_progress_report;
    
    mongo_config()
        : host(""),
          port(0),
          dbname(""),
          collection(""),
          username(""),
          password(""),
          auth_source(""),
          batch_size(DEFAULT_MONGO_BATCH_SIZE),
          connection_pool_size(DEFAULT_MONGO_CONNECTION_POOL_SIZE),
          timeout_ms(MONGO_TIMEOUT_MS),
          max_retries(MONGO_MAX_RETRIES),
          write_concern_level(WriteConcernLevel::PRIMARY),
          journal(false),
          wtimeout_ms(5000),
          create_indexes(true),
          drop_collection_before_write(false),
          write_metadata(false),
          enable_progress_report(true)
    {
    }
    
    // 解析 MongoDB 连接字符串
    // 严格格式：host:port:dbname:user:password:auth_source:collection（必须 7 部分）
    // 示例：localhost:27017:gis:user:pass:admin:china
    bool parse_connection_string(const std::string &conn_str) {
        std::vector<std::string> parts;
        std::string current;
        
        // 按 ':' 分割
        for (size_t i = 0; i < conn_str.size(); i++) {
            char c = conn_str[i];
            if (c == ':') {
                parts.push_back(current);
                current.clear();
            } else {
                current += c;
            }
        }
        parts.push_back(current);  // 最后一部分
        
        // 严格要求 7 部分
        if (parts.size() != 7) {
            fprintf(stderr, "Error: MongoDB connection string must have exactly 7 parts\n");
            fprintf(stderr, "Format: host:port:dbname:user:password:auth_source:collection\n");
            fprintf(stderr, "Example: localhost:27017:gis:user:pass:admin:china\n");
            fprintf(stderr, "Got %zu parts: %s\n", parts.size(), conn_str.c_str());
            return false;
        }
        
        // 解析各部分
        host = parts[0];
        if (!parts[1].empty()) {
            try {
                port = std::stoi(parts[1]);
            } catch (...) {
                fprintf(stderr, "Error: Invalid port number: %s\n", parts[1].c_str());
                return false;
            }
        } else {
            port = 27017;
        }
        dbname = parts[2];
        username = parts[3];
        password = parts[4];
        auth_source = parts[5];
        collection = parts[6];
        
        if (dbname.empty() || username.empty() || auth_source.empty() || collection.empty()) {
            fprintf(stderr, "Error: MongoDB connection string has empty required fields\n");
            fprintf(stderr, "Format: host:port:dbname:user:password:auth_source:collection\n");
            return false;
        }
        
        return true;
    }
    
    std::string uri() const {
        std::string uri_str = "mongodb://";
        
        if (!username.empty() && !password.empty()) {
            uri_str += username + ":" + password + "@";
        }
        
        uri_str += host + ":" + std::to_string(port) + "/";
        
        if (!dbname.empty()) {
            uri_str += dbname;
        }
        
        std::vector<std::string> opts;
        
        if (!auth_source.empty()) {
            opts.push_back("authSource=" + auth_source);
        }
        
        size_t actual_pool_size = std::min(connection_pool_size, MAX_MONGO_CONNECTION_POOL_SIZE);
        opts.push_back("maxPoolSize=" + std::to_string(actual_pool_size));
        opts.push_back("minPoolSize=1");
        
        opts.push_back("serverSelectionTimeoutMS=" + std::to_string(timeout_ms));
        opts.push_back("connectTimeoutMS=" + std::to_string(timeout_ms));
        opts.push_back("socketTimeoutMS=" + std::to_string(timeout_ms));
        
        switch (write_concern_level) {
            case WriteConcernLevel::NONE:
                opts.push_back("w=0");
                break;
            case WriteConcernLevel::PRIMARY:
                opts.push_back("w=1");
                break;
            case WriteConcernLevel::MAJORITY:
                opts.push_back("w=majority");
                break;
        }
        
        if (journal) {
            opts.push_back("journal=true");
        }
        
        if (wtimeout_ms > 0) {
            opts.push_back("wtimeoutMS=" + std::to_string(wtimeout_ms));
        }
        
        opts.push_back("retryReads=true");
        opts.push_back("retryWrites=true");
        
        if (!opts.empty()) {
            uri_str += "?";
            for (size_t i = 0; i < opts.size(); i++) {
                if (i > 0) uri_str += "&";
                uri_str += opts[i];
            }
        }
        
        return uri_str;
    }
};

// MongoDB 写入器类（线程安全）
class MongoWriter {
public:
    // 构造函数
    explicit MongoWriter(const mongo_config &cfg);
    
    // 析构函数 (noexcept - 析构函数中不抛出异常)
    ~MongoWriter() noexcept;
    
    // 初始化全局 mongocxx::instance（必须在 main() 中调用一次）
    static void initialize_global();
    
    // 获取或创建线程本地实例（线程安全）
    static MongoWriter* get_thread_local_instance(const mongo_config &cfg);
    
    // 销毁线程本地实例（在程序退出前调用）
    static void destroy_current_thread_instance();
    
    // 获取全局统计信息（所有 TLS 实例的总和）
    static size_t get_global_total_tiles();
    static size_t get_global_total_batches();
    static size_t get_global_total_retries();
    static size_t get_global_total_errors();
    
    // 初始化线程本地连接（每个工作线程调用）
    void initialize_thread();
    
    // 写入瓦片（线程安全，批量缓冲）
    void write_tile(int z, int x, int y, const char *data, size_t len);
    
    // 刷新所有缓冲区（在主要处理阶段完成后调用，noexcept - 异常在内部处理）
    void flush_all() noexcept;
    
    // 获取统计信息
    size_t getTotalTilesWritten() const { return total_tiles_written.load(); }
    size_t getTotalBatchesWritten() const { return total_batches_written.load(); }
    size_t getCurrentBufferSize() const { return batch_buffer.size(); }
    size_t getTotalRetries() const { return total_retries.load(); }
    size_t getTotalErrors() const { return total_errors.load(); }
    
    // 关闭连接（noexcept - 异常在内部处理）
    void close() noexcept;
    
    // 删除指定 zoom 级别的所有瓦片（用于重新生成）
    void erase_zoom(int z);

    // 写入元数据到 {collection}_metadata 集合
    void write_metadata(const std::string &json_metadata);

private:
    void flush_batch();
    void reconnect();
    void create_indexes_if_needed();
    void build_write_concern();
    
    struct tile_coords {
        int z, x, y;
    };
    
    mongo_config config;
    
    std::unique_ptr<mongocxx::client> client;
    mongocxx::collection collection;
    
    std::vector<bsoncxx::document::value> batch_buffer;
    std::vector<tile_coords> batch_coords;
    
    mongocxx::write_concern cached_wc;
    bool wc_initialized{false};
    bool use_upsert{true};
    
    // 统计信息（原子变量）
    std::atomic<size_t> total_tiles_written{0};
    std::atomic<size_t> total_batches_written{0};
    std::atomic<size_t> total_retries{0};
    std::atomic<size_t> total_errors{0};
    std::atomic<size_t> total_failed_batches{0};
    std::atomic<size_t> flush_failure_rounds{0};
    size_t consecutive_reconnects{0};

    static std::atomic<size_t> pending_writes;
    static constexpr size_t MAX_PENDING_WRITES = 5000;

    static std::unique_ptr<mongocxx::instance> global_instance;
    static std::atomic_flag initialized;
    static std::once_flag collection_drop_flag;
    static std::once_flag index_create_flag;
};

// Each worker thread must call destroy_current_thread_instance() on its own
// exit path to ensure its buffered tiles are flushed and statistics are
// accumulated into the global counters.

#endif // MONGO_HPP
