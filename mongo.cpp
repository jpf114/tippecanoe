#include "mongo.hpp"
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#include "errors.hpp"

// 全局静态成员变量初始化
std::unique_ptr<mongocxx::instance> MongoWriter::global_instance = nullptr;
std::atomic_flag MongoWriter::initialized = ATOMIC_FLAG_INIT;
std::atomic_flag MongoWriter::collection_dropped = ATOMIC_FLAG_INIT;

// 线程本地存储的 MongoWriter 实例
thread_local static std::unique_ptr<MongoWriter> tls_mongo_writer;

// 全局统计信息（所有线程累计）
static std::atomic<size_t> global_total_tiles{0};
static std::atomic<size_t> global_total_batches{0};
static std::atomic<size_t> global_total_retries{0};
static std::atomic<size_t> global_total_errors{0};

/**
 * @brief 获取线程本地的 MongoWriter 实例
 * 
 * 使用线程本地存储（TLS）模式，每个线程独立的 MongoDB 连接和缓冲区
 * 避免锁竞争，提高并发性能
 * 
 * @param cfg MongoDB 配置参数
 * @return MongoWriter* 线程本地的 MongoWriter 实例指针
 */
MongoWriter* MongoWriter::get_thread_local_instance(const mongo_config &cfg) {
    if (!tls_mongo_writer) {
        tls_mongo_writer = std::make_unique<MongoWriter>(cfg);
        tls_mongo_writer->initialize_thread();
    }
    return tls_mongo_writer.get();
}

/**
 * @brief 获取全局累计写入的瓦片总数
 * 
 * @return size_t 全局累计写入的瓦片数量
 */
size_t MongoWriter::get_global_total_tiles() {
    return global_total_tiles.load();
}

/**
 * @brief 获取全局累计写入的批次总数
 * 
 * @return size_t 全局累计写入的批次数量
 */
size_t MongoWriter::get_global_total_batches() {
    return global_total_batches.load();
}

/**
 * @brief 获取全局累计重试次数
 * 
 * @return size_t 全局累计重试次数
 */
size_t MongoWriter::get_global_total_retries() {
    return global_total_retries.load();
}

/**
 * @brief 获取全局累计错误次数
 * 
 * @return size_t 全局累计错误次数
 */
size_t MongoWriter::get_global_total_errors() {
    return global_total_errors.load();
}

/**
 * @brief 销毁线程本地的 MongoWriter 实例
 * 
 * 在线程结束时调用，刷新所有未写入的数据，更新全局统计信息，释放连接资源
 */
void MongoWriter::destroy_thread_local_instances() {
    if (tls_mongo_writer) {
        tls_mongo_writer->flush_all();
        
        global_total_tiles += tls_mongo_writer->getTotalTilesWritten();
        global_total_batches += tls_mongo_writer->getTotalBatchesWritten();
        global_total_retries += tls_mongo_writer->getTotalRetries();
        global_total_errors += tls_mongo_writer->getTotalErrors();
        
        tls_mongo_writer->close();
        tls_mongo_writer.reset();
    }
}

extern int quiet;

/**
 * @brief MongoWriter 构造函数
 * 
 * 初始化 MongoDB 写入器，验证配置参数，设置批次大小范围
 * 
 * @param cfg MongoDB 配置结构，包含连接信息和性能参数
 * @throws std::runtime_error 当数据库名为空时抛出异常
 */
MongoWriter::MongoWriter(const mongo_config &cfg)
    : config(cfg)
{
    if (config.dbname.empty()) {
        throw std::runtime_error("MongoDB database name is required");
    }
    
    if (config.batch_size < MIN_MONGO_BATCH_SIZE) {
        config.batch_size = MIN_MONGO_BATCH_SIZE;
    } else if (config.batch_size > MAX_MONGO_BATCH_SIZE) {
        config.batch_size = MAX_MONGO_BATCH_SIZE;
    }
    
    batch_buffer.reserve(config.batch_size);
}

/**
 * @brief MongoWriter 析构函数
 * 
 * 确保所有数据都被刷新到数据库，释放连接资源
 * 使用 noexcept 保证异常安全
 */
MongoWriter::~MongoWriter() noexcept
{
    flush_all();
    
    close();
}

/**
 * @brief 初始化全局 MongoDB 实例
 * 
 * 使用原子操作确保只初始化一次，线程安全
 */
void MongoWriter::initialize_global()
{
    if (!initialized.test_and_set(std::memory_order_acquire)) {
        global_instance = std::make_unique<mongocxx::instance>();
    }
}

/**
 * @brief 初始化线程本地的 MongoDB 连接
 * 
 * 创建数据库连接，获取集合引用，清空集合（如果配置），创建索引
 * 
 * @throws std::exception 连接失败时抛出异常
 */
void MongoWriter::initialize_thread()
{
    try {
        mongocxx::uri uri(config.uri());
        client = std::make_unique<mongocxx::client>(uri);
        
        collection = (*client)[config.dbname][config.collection];
        
        if (config.drop_collection_before_write) {
            if (!collection_dropped.test_and_set(std::memory_order_acquire)) {
                try {
                    collection.drop();
                    if (!quiet) {
                        fprintf(stderr, "MongoDB: 已清空集合 %s.%s\n", 
                                config.dbname.c_str(), config.collection.c_str());
                    }
                } catch (const std::exception &e) {
                    fprintf(stderr, "警告：清空 MongoDB 集合 %s.%s 失败：%s\n", 
                            config.dbname.c_str(), config.collection.c_str(), e.what());
                    collection_dropped.clear(std::memory_order_release);
                }
            }
        }
        
        if (config.create_indexes) {
            create_indexes_if_needed();
        }
        
        if (config.enable_progress_report && !quiet) {
            fprintf(stderr, "MongoDB 连接已初始化：%s.%s (连接池：%zu, 写确认：%d%s)\n", 
                    config.dbname.c_str(), config.collection.c_str(), config.connection_pool_size, 
                    static_cast<int>(config.write_concern_level),
                    config.drop_collection_before_write ? ", 写入前清空" : "");
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "错误：MongoDB 连接初始化失败：%s\n", e.what());
        throw;
    }
}

/**
 * @brief 写入单个瓦片数据
 * 
 * 将瓦片数据添加到批量缓冲区，达到批次大小时自动刷新
 * 支持自动重试机制，失败时自动重新连接
 * 
 * @param z Zoom 级别
 * @param x X 坐标（XYZ 坐标系）
 * @param y Y 坐标（XYZ 坐标系）
 * @param data 瓦片数据指针
 * @param len 瓦片数据长度
 * @throws std::exception 重试次数用尽后抛出异常
 */
void MongoWriter::write_tile(int z, int x, int y, const char *data, size_t len)
{
    int attempts = 0;
    
    while (attempts < config.max_retries) {
        try {
            auto doc = bsoncxx::builder::stream::document{}
                << "x" << x
                << "y" << y
                << "z" << z
                << "d" << bsoncxx::types::b_binary{
                    bsoncxx::binary_sub_type::k_binary,
                    static_cast<uint32_t>(len),
                    reinterpret_cast<const uint8_t*>(data)
                }
                << bsoncxx::builder::stream::finalize;
            
            batch_buffer.push_back(std::move(doc));
            
            if (batch_buffer.size() >= config.batch_size) {
                flush_batch();
            }
            
            return;
            
        } catch (const std::exception &e) {
            attempts++;
            total_errors++;
            
            if (!quiet) {
                fprintf(stderr, "MongoDB 写入失败（第 %d/%d 次尝试）: %s\n", 
                        attempts, config.max_retries, e.what());
            }
            
            if (attempts >= config.max_retries) {
                fprintf(stderr, "错误：MongoDB 写入失败，已尝试 %d 次。数据可能丢失。\n", 
                        config.max_retries);
                throw;
            }
            
            try {
                reconnect();
                total_retries++;
            } catch (const std::exception &reconnect_e) {
                if (!quiet) {
                    fprintf(stderr, "MongoDB 重新连接失败：%s\n", reconnect_e.what());
                }
            }
        }
    }
}

/**
 * @brief 刷新所有待写入的数据
 * 
 * 将批量缓冲区中的所有瓦片数据写入数据库
 * 使用 noexcept 保证异常安全，捕获所有异常
 */
void MongoWriter::flush_all() noexcept
{
    try {
        if (!batch_buffer.empty()) {
            flush_batch();
        }
    } catch (const std::exception &e) {
        total_errors++;
        if (!quiet) {
            fprintf(stderr, "警告：flush_all 失败：%s\n", e.what());
        }
    } catch (...) {
        total_errors++;
        if (!quiet) {
            fprintf(stderr, "警告：flush_all 失败（未知错误）\n");
        }
    }
}

/**
 * @brief 关闭 MongoDB 连接
 * 
 * 刷新所有数据，释放客户端连接
 * 使用 noexcept 保证异常安全
 */
void MongoWriter::close() noexcept
{
    try {
        flush_all();
    } catch (...) {
        total_errors++;
    }
    
    try {
        if (client) {
            client.reset();
        }
    } catch (...) {
        total_errors++;
    }
}

/**
 * @brief 刷新批量缓冲区
 * 
 * 将批量缓冲区中的所有瓦片文档批量插入到 MongoDB 集合
 * 支持写确认配置、日志配置、超时配置
 * 失败时自动重试，使用指数退避策略
 * 
 * @throws std::exception 重试次数用尽后抛出异常
 */
void MongoWriter::flush_batch()
{
    if (batch_buffer.empty()) {
        return;
    }
    
    int attempts = 0;
    bool success = false;
    
    while (!success && attempts < config.max_retries) {
        try {
            mongocxx::options::insert insert_opts;
            insert_opts.bypass_document_validation(false);
            insert_opts.ordered(false);
            
            mongocxx::write_concern wc;
            switch (config.write_concern_level) {
                case WriteConcernLevel::NONE:
                    wc.acknowledge_level(mongocxx::write_concern::level::k_unacknowledged);
                    break;
                case WriteConcernLevel::PRIMARY:
                    wc.acknowledge_level(mongocxx::write_concern::level::k_acknowledged);
                    wc.nodes(1);
                    break;
                case WriteConcernLevel::MAJORITY:
                    wc.acknowledge_level(mongocxx::write_concern::level::k_majority);
                    break;
            }
            
            if (config.journal) {
                wc.journal(true);
            }
            
            if (config.wtimeout_ms > 0) {
                wc.timeout(std::chrono::milliseconds(config.wtimeout_ms));
            }
            
            mongocxx::options::bulk_write bulk_opts;
            bulk_opts.ordered(false); // 使用非顺序执行以获得更高性能
            auto bulk = collection.create_bulk_write(bulk_opts);

            for (const auto& doc : batch_buffer) {
                bsoncxx::document::view view = doc.view();
                auto filter = bsoncxx::builder::stream::document{} 
                    << "z" << view["z"].get_int32()
                    << "x" << view["x"].get_int32()
                    << "y" << view["y"].get_int32()
                    << bsoncxx::builder::stream::finalize;
                
                mongocxx::model::replace_one upsert_op(filter.view(), view);
                upsert_op.upsert(true);
                bulk.append(upsert_op);
            }

            auto result = bulk.execute();
            
            total_tiles_written += batch_buffer.size();
            total_batches_written++;
            
            batch_buffer.clear();
            success = true;
            
        } catch (const std::exception &e) {
            attempts++;
            total_errors++;
            
            if (!quiet) {
                fprintf(stderr, "MongoDB flush_batch failed (attempt %d/%d): %s\n", 
                        attempts, config.max_retries, e.what());
            }
            
            if (attempts >= config.max_retries) {
                total_failed_batches++;
                fprintf(stderr, "Error: MongoDB flush_batch failed after %d attempts. %zu tiles may be lost.\n", 
                        config.max_retries, batch_buffer.size());
                if (attempts >= config.max_retries * 2) {
                    batch_buffer.clear();
                }
                throw;
            }
            
            try {
                reconnect();
                total_retries++;
            } catch (const std::exception &reconnect_e) {
                if (!quiet) {
                    fprintf(stderr, "MongoDB reconnect failed during flush_batch: %s\n", reconnect_e.what());
                }
            }
        }
    }
}

/**
 * @brief 重新连接 MongoDB
 * 
 * 使用指数退避策略等待后重新建立数据库连接
 * 等待时间：100ms * 2^retries，最大 5 秒
 * 
 * @throws std::exception 重新连接失败时抛出异常
 */
void MongoWriter::reconnect()
{
    int wait_ms = 100 * (1 << total_retries.load());
    wait_ms = std::min(wait_ms, 5000);
    
    if (!quiet && wait_ms > 100) {
        fprintf(stderr, "MongoDB reconnecting in %d ms...\n", wait_ms);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    
    try {
        mongocxx::uri uri(config.uri());
        client = std::make_unique<mongocxx::client>(uri);
        collection = (*client)[config.dbname][config.collection];
        
        if (!quiet) {
            fprintf(stderr, "MongoDB reconnected successfully\n");
        }
        
    } catch (const std::exception &e) {
        fprintf(stderr, "MongoDB reconnect failed: %s\n", e.what());
        throw;
    }
}

/**
 * @brief 创建 MongoDB 索引（如果需要）
 * 
 * 检查并创建以下索引：
 * 1. 唯一索引 (z, x, y) - 防止重复瓦片
 * 2. 查询索引 (z) - 加速按 Zoom 级别查询
 */
void MongoWriter::create_indexes_if_needed()
{
    try {
        auto index_view = collection.indexes();
        
        bool unique_index_exists = false;
        bool zoom_index_exists = false;
        
        try {
            for (const auto &index : index_view.list()) {
                auto name_element = index["name"];
                if (name_element && 
                    name_element.get_string().value == bsoncxx::string::view_or_value("tile_coords_unique")) {
                    unique_index_exists = true;
                }
                if (name_element && 
                    name_element.get_string().value == bsoncxx::string::view_or_value("zoom_level")) {
                    zoom_index_exists = true;
                }
            }
        } catch (const std::exception &e) {
            if (!quiet) {
                fprintf(stderr, "Warning: Could not list MongoDB indexes: %s. Will attempt to create.\n", e.what());
            }
        }
        
        if (!unique_index_exists) {
            try {
                auto keys = bsoncxx::builder::stream::document{}
                    << "z" << 1
                    << "x" << 1
                    << "y" << 1
                    << bsoncxx::builder::stream::finalize;
                
                auto options = bsoncxx::builder::stream::document{}
                    << "unique" << true
                    << "name" << "tile_coords_unique"
                    << bsoncxx::builder::stream::finalize;
                
                index_view.create_one(keys.view(), options.view());
                
                if (!quiet) {
                    fprintf(stderr, "Created unique index on (z, x, y) for %s.%s\n", 
                            config.dbname.c_str(), config.collection.c_str());
                }
            } catch (const std::exception &e) {
                if (!quiet) {
                    fprintf(stderr, "Note: Unique index on (z, x, y) may already exist: %s\n", e.what());
                }
            }
        }
        
        if (!zoom_index_exists) {
            try {
                auto keys = bsoncxx::builder::stream::document{}
                    << "z" << 1
                    << bsoncxx::builder::stream::finalize;
                
                auto options = bsoncxx::builder::stream::document{}
                    << "name" << "zoom_level"
                    << bsoncxx::builder::stream::finalize;
                
                index_view.create_one(keys.view(), options.view());
                
                if (!quiet) {
                    fprintf(stderr, "Created index on z for %s.%s\n", 
                            config.dbname.c_str(), config.collection.c_str());
                }
            } catch (const std::exception &e) {
                if (!quiet) {
                    fprintf(stderr, "Note: Index on z may already exist: %s\n", e.what());
                }
            }
        }
        
    } catch (const std::exception &e) {
        fprintf(stderr, "Warning: Failed to create MongoDB indexes: %s\n", e.what());
    }
}

/**
 * @brief 删除指定 Zoom 级别的所有瓦片
 * 
 * 用于增量更新或重新生成特定层级的瓦片
 * 
 * @param z 要删除的 Zoom 级别
 */
void MongoWriter::erase_zoom(int z)
{
    try {
        auto filter = bsoncxx::builder::stream::document{}
            << "z" << z
            << bsoncxx::builder::stream::finalize;
        
        auto result = collection.delete_many(filter.view());
        
        if (!quiet) {
            fprintf(stderr, "MongoDB: 删除了 z=%d 的 %d 个瓦片\n", 
                    z, static_cast<int>(result->deleted_count()));
        }
    } catch (const std::exception &e) {
        if (!quiet) {
            fprintf(stderr, "警告：删除 MongoDB z=%d 的瓦片失败：%s\n", 
                    z, e.what());
        }
    } catch (...) {
        if (!quiet) {
            fprintf(stderr, "警告：删除 MongoDB z=%d 的瓦片失败（未知错误）\n", z);
        }
    }
}
