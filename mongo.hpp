#ifndef MONGO_HPP
#define MONGO_HPP

#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/document/view.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>
#include <mongocxx/options/insert.hpp>
#include <mongocxx/options/replace.hpp>
#include <mongocxx/write_concern.hpp>
#include <mongocxx/options/client.hpp>

// MongoDB 性能优化常量
constexpr size_t DEFAULT_MONGO_BATCH_SIZE = 100;     // 默认批量插入大小
constexpr size_t MAX_MONGO_BATCH_SIZE = 1000;        // 最大批量大小
constexpr size_t MIN_MONGO_BATCH_SIZE = 10;          // 最小批量大小
constexpr int MONGO_TIMEOUT_MS = 30000;              // 默认超时时间（毫秒）
constexpr int MONGO_MAX_RETRIES = 3;                 // 最大重试次数
constexpr int DEFAULT_WRITE_CONCERN = 1;             // 默认写确认级别 (0: 无确认，1: 主节点，2: majority)
constexpr size_t MAX_CONNECTION_POOL_SIZE = 50;      // 最大连接池大小（防止资源耗尽）
constexpr size_t DEFAULT_CONNECTION_POOL_SIZE = 10;  // 默认连接池大小

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
    
    // 监控参数
    bool enable_progress_report;
    
    mongo_config()
        : host("localhost"),
          port(27017),
          dbname(""),
          collection("tiles"),
          username(""),
          password(""),
          auth_source("admin"),
          batch_size(DEFAULT_MONGO_BATCH_SIZE),
          connection_pool_size(DEFAULT_CONNECTION_POOL_SIZE),
          timeout_ms(MONGO_TIMEOUT_MS),
          max_retries(MONGO_MAX_RETRIES),
          write_concern_level(WriteConcernLevel::PRIMARY),
          journal(false),
          wtimeout_ms(5000),
          create_indexes(true),
          drop_collection_before_write(false),
          enable_progress_report(true)
    {
    }
    
    // 解析 MongoDB 连接字符串
    // 支持格式：
    //   - 5 部分：host:port:dbname:user:password
    //   - 6 部分：host:port:dbname:user:password:collection
    //   - 7 部分：host:port:dbname:user:password:auth_source:collection
    //   - 8 部分：host:port:dbname:user:password:auth_source:collection:drop
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
        
        // 至少需要 host:port:dbname:user:password (5 部分)
        if (parts.size() < 5) {
            return false;
        }
        
        // 解析各部分
        host = parts[0];
        if (!parts[1].empty()) {
            try {
                port = std::stoi(parts[1]);
            } catch (...) {
                port = 27017;
            }
        }
        dbname = parts[2];
        username = parts[3];
        password = parts[4];
        
        // 可选的 auth_source 和 collection
        if (parts.size() >= 7 && !parts[5].empty() && !parts[6].empty()) {
            // 7 部分格式：host:port:dbname:user:password:auth_source:collection
            auth_source = parts[5];
            collection = parts[6];
        } else if (parts.size() >= 6 && !parts[5].empty()) {
            // 6 部分格式：host:port:dbname:user:password:collection
            collection = parts[5];
            // auth_source 使用默认值 "admin"
        }
        
        // 可选的 drop 标志（第 8 部分）
        // 8 部分格式：host:port:dbname:user:password:auth_source:collection:drop
        if (parts.size() >= 8) {
            std::string drop_str = parts[7];
            // 支持 "drop"、"true"、"1"、"yes" 等值
            if (drop_str == "drop" || drop_str == "true" || 
                drop_str == "1" || drop_str == "yes") {
                drop_collection_before_write = true;
            }
        }
        
        return !dbname.empty() && !username.empty();
    }
    
    // 生成 MongoDB 连接 URI（包含连接池、超时、写确认等配置）
    std::string uri() const {
        std::string uri_str = "mongodb://";
        
        if (!username.empty() && !password.empty()) {
            uri_str += username + ":" + password + "@";
        }
        
        uri_str += host + ":" + std::to_string(port) + "/";
        
        if (!dbname.empty()) {
            uri_str += dbname;
        }
        
        // 添加连接选项
        std::string options;
        if (!auth_source.empty()) {
            options += "authSource=" + auth_source;
        }
        
        // 连接池配置（限制最大值防止资源耗尽）
        size_t actual_pool_size = std::min(connection_pool_size, MAX_CONNECTION_POOL_SIZE);
        options += "&maxPoolSize=" + std::to_string(actual_pool_size);
        options += "&minPoolSize=1";
        
        // 超时配置
        options += "&serverSelectionTimeoutMS=" + std::to_string(timeout_ms);
        options += "&connectTimeoutMS=" + std::to_string(timeout_ms);
        options += "&socketTimeoutMS=" + std::to_string(timeout_ms);
        
        // 写确认配置
        switch (write_concern_level) {
            case WriteConcernLevel::NONE:
                options += "&w=0";
                break;
            case WriteConcernLevel::PRIMARY:
                options += "&w=1";
                break;
            case WriteConcernLevel::MAJORITY:
                options += "&w=majority";
                break;
        }
        
        if (journal) {
            options += "&journal=true";
        }
        
        if (wtimeout_ms > 0) {
            options += "&wtimeoutMS=" + std::to_string(wtimeout_ms);
        }
        
        // 添加重试读取配置
        options += "&retryReads=true";
        options += "&retryWrites=true";
        
        if (!options.empty()) {
            uri_str += "?" + options;
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
    static void destroy_thread_local_instances();
    
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

private:
    // 批量刷新（带重试机制）
    void flush_batch();
    
    // 重新连接（带指数退避）
    void reconnect();
    
    // 创建索引
    void create_indexes_if_needed();
    
    // 配置
    mongo_config config;
    
    // 线程本地客户端和集合
    std::unique_ptr<mongocxx::client> client;
    mongocxx::collection collection;
    
    // 批量缓冲区（线程本地）
    std::vector<bsoncxx::document::value> batch_buffer;
    
    // 统计信息（原子变量）
    std::atomic<size_t> total_tiles_written{0};
    std::atomic<size_t> total_batches_written{0};
    std::atomic<size_t> total_retries{0};
    std::atomic<size_t> total_errors{0};
    std::atomic<size_t> total_failed_batches{0};  // 新增：失败的批量写入次数
    
    // 全局实例（单例）
    static std::unique_ptr<mongocxx::instance> global_instance;
    static std::atomic_flag initialized;
    static std::atomic_flag collection_dropped;  // 确保只清空一次集合
};

// TLS（线程本地存储）实例管理
// 注意：TLS 实例由 thread_local 变量自动管理，不需要显式注册表
// 每个线程的 tls_mongo_writer 在该线程结束时自动销毁
// 建议在每个工作线程退出前显式调用 destroy_thread_local_instances() 确保数据刷新

#endif // MONGO_HPP
