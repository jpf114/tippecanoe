#include "mongo.hpp"
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#include "errors.hpp"

// 静态成员初始化
std::unique_ptr<mongocxx::instance> MongoWriter::global_instance = nullptr;
std::atomic_flag MongoWriter::initialized = ATOMIC_FLAG_INIT;
std::atomic_flag MongoWriter::collection_dropped = ATOMIC_FLAG_INIT;

// 线程本地 MongoWriter 实例
thread_local static std::unique_ptr<MongoWriter> tls_mongo_writer;

// 获取线程本地实例
MongoWriter* MongoWriter::get_thread_local_instance(const mongo_config &cfg) {
    if (!tls_mongo_writer) {
        tls_mongo_writer = std::make_unique<MongoWriter>(cfg);
        tls_mongo_writer->initialize_thread();
    }
    return tls_mongo_writer.get();
}

// 销毁线程本地实例
void MongoWriter::destroy_thread_local_instances() {
    // TLS 对象会在线程结束时自动销毁
    // 这个函数确保在当前线程显式销毁 TLS 对象
    // 注意：必须在每个使用 MongoDB 的工作线程中调用
    // 主线程应该在退出前调用此函数
    if (tls_mongo_writer) {
        tls_mongo_writer->flush_all();
        tls_mongo_writer->close();
        tls_mongo_writer.reset();
    }
}

// 外部 quiet 变量声明（来自 maindb.cpp，用于控制日志输出）
extern int quiet;

MongoWriter::MongoWriter(const mongo_config &cfg)
    : config(cfg)
{
    // 验证配置参数
    if (config.dbname.empty()) {
        throw std::runtime_error("MongoDB database name is required");
    }
    
    // 限制批量大小在合理范围内
    if (config.batch_size < MIN_MONGO_BATCH_SIZE) {
        config.batch_size = MIN_MONGO_BATCH_SIZE;
    } else if (config.batch_size > MAX_MONGO_BATCH_SIZE) {
        config.batch_size = MAX_MONGO_BATCH_SIZE;
    }
    
    // 预分配缓冲区内存，提高性能
    batch_buffer.reserve(config.batch_size);
}

MongoWriter::~MongoWriter() noexcept
{
    // 确保所有数据已刷新（noexcept 版本）
    flush_all();
    
    // 关闭连接（noexcept 版本）
    close();
}

void MongoWriter::initialize_global()
{
    // 双重检查锁定模式初始化全局实例
    if (!initialized.test_and_set(std::memory_order_acquire)) {
        global_instance = std::make_unique<mongocxx::instance>();
    }
}

void MongoWriter::initialize_thread()
{
    try {
        // 创建线程本地客户端（URI 中已包含连接池和写确认配置）
        mongocxx::uri uri(config.uri());
        client = std::make_unique<mongocxx::client>(uri);
        
        // 获取集合引用
        collection = (*client)[config.dbname][config.collection];
        
        // 如果需要，清空集合（在创建索引之前，且只清空一次）
        if (config.drop_collection_before_write) {
            // 使用原子操作确保只有一个线程执行清空
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
                    // 清除标志允许重试
                    collection_dropped.clear(std::memory_order_release);
                }
            }
        }
        
        // 如果需要，创建索引
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
        throw;  // 抛出异常由上层处理
    }
}

void MongoWriter::write_tile(int z, int x, int y, const char *data, size_t len)
{
    int attempts = 0;
    
    while (attempts < config.max_retries) {
        try {
            // 构建 MongoDB document
            // 使用 XYZ 坐标系（与 mbtiles 一致），不翻转 Y 坐标
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
            
            // 添加到批量缓冲区（使用 std::move 转移所有权）
            batch_buffer.push_back(std::move(doc));
            
            // 检查是否达到批量阈值
            if (batch_buffer.size() >= config.batch_size) {
                flush_batch();
            }
            
            return;  // 成功
            
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
                throw;  // 抛出异常由上层处理
            }
            
            // 重试前重新连接（带指数退避）
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

void MongoWriter::flush_all() noexcept
{
    try {
        // 刷新所有批量缓冲区
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
    
    // 输出统计信息
    if (config.enable_progress_report && !quiet) {
        fprintf(stderr, "MongoDB: 已写入 %zu 个瓦片，共 %zu 个批次\n", 
                total_tiles_written.load(), total_batches_written.load());
        if (total_retries.load() > 0) {
            fprintf(stderr, "MongoDB: 重试 %zu 次，错误 %zu 次\n", 
                    total_retries.load(), total_errors.load());
        }
    }
}

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

void MongoWriter::flush_batch()
{
    if (batch_buffer.empty()) {
        return;
    }
    
    int attempts = 0;
    bool success = false;
    
    while (!success && attempts < config.max_retries) {
        try {
            // 配置插入选项和写确认
            mongocxx::options::insert insert_opts;
            insert_opts.bypass_document_validation(false);
            insert_opts.ordered(false);  // 无序插入，提高性能
            
            // 设置写确认级别
            mongocxx::write_concern wc;
            switch (config.write_concern_level) {
                case WriteConcernLevel::NONE:
                    // w:0 - 无确认
                    wc.acknowledge_level(mongocxx::write_concern::level::k_unacknowledged);
                    break;
                case WriteConcernLevel::PRIMARY:
                    // w:1 - 等待主节点确认
                    wc.acknowledge_level(mongocxx::write_concern::level::k_acknowledged);
                    wc.nodes(1);
                    break;
                case WriteConcernLevel::MAJORITY:
                    // w:"majority" - 等待多数节点确认
                    wc.acknowledge_level(mongocxx::write_concern::level::k_majority);
                    break;
            }
            
            // 设置 journal
            if (config.journal) {
                wc.journal(true);
            }
            
            // 设置 wtimeout
            if (config.wtimeout_ms > 0) {
                wc.timeout(std::chrono::milliseconds(config.wtimeout_ms));
            }
            
            insert_opts.write_concern(wc);
            
            // 执行批量插入
            auto result = collection.insert_many(batch_buffer, insert_opts);
            
            // 更新统计
            total_tiles_written += batch_buffer.size();
            total_batches_written++;
            
            // 清空缓冲区
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
                // 不清空缓冲区，保留数据以便后续可能的恢复
                // 但为了避免内存无限增长，在多次失败后清空
                if (attempts >= config.max_retries * 2) {
                    batch_buffer.clear();
                }
                throw;  // 重新抛出异常
            }
            
            // 重试前重新连接（带指数退避）
            try {
                reconnect();
                total_retries++;
            } catch (const std::exception &reconnect_e) {
                if (!quiet) {
                    fprintf(stderr, "MongoDB reconnect failed during flush_batch: %s\n", reconnect_e.what());
                }
                // 继续重试循环
            }
        }
    }
}

void MongoWriter::reconnect()
{
    // 指数退避策略：第 n 次重试等待 100ms * 2^(n-1)
    int wait_ms = 100 * (1 << total_retries.load());
    wait_ms = std::min(wait_ms, 5000);  // 最多等待 5 秒
    
    if (!quiet && wait_ms > 100) {
        fprintf(stderr, "MongoDB reconnecting in %d ms...\n", wait_ms);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    
    try {
        // 重新创建客户端
        mongocxx::uri uri(config.uri());
        client = std::make_unique<mongocxx::client>(uri);
        collection = (*client)[config.dbname][config.collection];
        
        if (!quiet) {
            fprintf(stderr, "MongoDB reconnected successfully\n");
        }
        
    } catch (const std::exception &e) {
        fprintf(stderr, "MongoDB reconnect failed: %s\n", e.what());
        throw;  // 重新抛出，由上层处理
    }
}

void MongoWriter::create_indexes_if_needed()
{
    try {
        // 获取索引视图
        auto index_view = collection.indexes();
        
        // 使用 try-catch 包裹索引检查和创建，提高健壮性
        bool unique_index_exists = false;
        bool zoom_index_exists = false;
        
        try {
            // 检查唯一索引是否已存在
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
            // 索引列表获取失败，尝试直接创建（如果已存在会失败，但无害）
            if (!quiet) {
                fprintf(stderr, "Warning: Could not list MongoDB indexes: %s. Will attempt to create.\n", e.what());
            }
        }
        
        if (!unique_index_exists) {
            try {
                // 创建唯一索引：{z: 1, x: 1, y: 1}
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
                // 索引可能已存在（竞态条件），忽略错误
                if (!quiet) {
                    fprintf(stderr, "Note: Unique index on (z, x, y) may already exist: %s\n", e.what());
                }
            }
        }
        
        if (!zoom_index_exists) {
            try {
                // 创建缩放级别索引：{z: 1}
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
                // 索引可能已存在（竞态条件），忽略错误
                if (!quiet) {
                    fprintf(stderr, "Note: Index on z may already exist: %s\n", e.what());
                }
            }
        }
        
    } catch (const std::exception &e) {
        fprintf(stderr, "Warning: Failed to create MongoDB indexes: %s\n", e.what());
        // 索引创建失败不影响主流程
    }
}

// 删除指定 zoom 级别的所有瓦片
void MongoWriter::erase_zoom(int z)
{
    try {
        // 删除指定 z 值的所有文档
        auto filter = bsoncxx::builder::stream::document{}
            << "z" << z
            << bsoncxx::builder::stream::finalize;
        
        auto result = collection.delete_many(filter.view());
        
        if (!quiet) {
            fprintf(stderr, "MongoDB: 删除了 z=%d 的 %lld 个瓦片\n", 
                    z, result->deleted_count());
        }
    } catch (const std::exception &e) {
        if (!quiet) {
            fprintf(stderr, "警告：删除 MongoDB z=%d 的瓦片失败：%s\n", 
                    z, e.what());
        }
        // 删除失败不阻止主流程
    } catch (...) {
        if (!quiet) {
            fprintf(stderr, "警告：删除 MongoDB z=%d 的瓦片失败（未知错误）\n", z);
        }
    }
}
