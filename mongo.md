# Tippecanoe 的 MongoDB 支持

## 概述

本文档描述了 Tippecanoe 的 MongoDB 支持功能，使其能够将生成的矢量瓦片直接存储到 MongoDB 数据库中，提供灵活、可扩展的瓦片存储方案。

## 已实现的功能

1. **直接数据库连接** - 使用 mongocxx 驱动安全连接到 MongoDB 数据库
2. **批量写入优化** - 支持批量插入，提高写入性能
3. **线程安全** - 线程本地存储（TLS）设计，支持多线程并发写入
4. **自动索引** - 自动创建唯一索引和查询索引，优化查询性能
5. **错误重试** - 自动重试机制提高稳定性
6. **进度报告** - 实时显示数据处理进度
7. **写确认配置** - 支持多种写确认级别（无确认、主节点确认、多数节点确认）
8. **连接池管理** - 可配置连接池大小，平衡性能和资源消耗
9. **集合管理** - 支持写入前清空集合，方便重新生成数据
10. **坐标系统兼容** - 使用 XYZ 坐标系（与 mbtiles 完全兼容）

## 技术实现

### 修改/添加的文件

1. **mongo.hpp** - 定义 MongoDB 配置结构和 MongoWriter 类，包含性能优化参数
2. **mongo.cpp** - 实现 MongoDB 连接、批量写入、错误重试和线程管理
3. **maindb.cpp** - 添加 MongoDB 命令行选项和集成逻辑
4. **Makefile** - 添加 MongoDB C++ 驱动依赖

### 关键组件

#### MongoDB 配置

```cpp
struct mongo_config {
    // 连接参数
    std::string host;              // 数据库主机
    int port;                      // 数据库端口
    std::string dbname;            // 数据库名称
    std::string collection;        // 集合名称
    std::string username;          // 用户名
    std::string password;          // 密码
    std::string auth_source;       // 认证源
    
    // 性能参数
    size_t batch_size;             // 每批次写入数量（默认 100）
    size_t connection_pool_size;   // 连接池大小（默认 10）
    int timeout_ms;                // 超时时间（默认 30000ms）
    int max_retries;               // 最大重试次数（默认 3）
    
    // 写入参数
    WriteConcernLevel write_concern_level;  // 写确认级别
    bool journal;                            // 是否启用日志
    int wtimeout_ms;                         // 写确认超时
    
    // 索引参数
    bool create_indexes;         // 是否创建索引
    bool drop_collection_before_write;  // 写入前是否清空集合
    bool enable_progress_report; // 是否启用进度报告
};
```

#### MongoWriter 类

`MongoWriter` 类处理：
- 使用 mongocxx 的安全数据库连接
- **线程本地存储（TLS）**：每个工作线程独立的 MongoDB 连接
- **批量写入**：累积瓦片到批量缓冲区，达到阈值后批量插入
- **错误重试**：失败时自动重试，带指数退避策略
- **索引管理**：自动创建唯一索引 `(z, x, y)` 和查询索引 `z`
- **进度监控**：实时统计写入数量和批次
- **内存管理**：预分配缓冲区内存，提高性能
- **异常安全**：析构函数和关键函数使用 `noexcept` 保证异常安全

#### 瓦片数据结构

MongoDB 中每个瓦片存储为一个文档：
```javascript
{
  "x": <int>,        // X 坐标（XYZ 坐标系）
  "y": <int>,        // Y 坐标（XYZ 坐标系，不翻转）
  "z": <int>,        // Zoom 级别
  "d": <binary>      // 瓦片数据（二进制）
}
```

**重要**：坐标系统使用 **XYZ（Google/OSM）**，与 mbtiles 完全兼容，不做 Y 坐标翻转。

### 性能优化

#### 核心优化策略

1. **线程本地存储（TLS）架构**
   - 每个工作线程独立的 MongoDB 连接和缓冲区
   - 避免锁竞争，提高并发性能
   - 自动管理连接生命周期

2. **批量写入优化**
   - 默认每批次 100 个瓦片（可配置范围 10-1000）
   - 使用 `insert_many` 批量插入
   - 无序插入（`ordered: false`）提高性能

3. **连接池管理**
   - 默认连接池大小：10
   - 最大连接池大小：50（防止资源耗尽）
   - 自动连接复用，减少连接开销

4. **错误重试机制**
   - 失败时自动重试（最多 3 次）
   - 指数退避策略：100ms, 200ms, 400ms, 800ms, 1600ms...
   - 最大等待时间：5 秒

5. **写确认配置**
   - **NONE (w:0)**: 无确认，最快但不安全
   - **PRIMARY (w:1)**: 等待主节点确认（默认）
   - **MAJORITY (w:"majority")**: 等待多数节点确认，最安全

6. **索引优化**
   - 唯一索引 `(z, x, y)`: 防止重复瓦片
   - 查询索引 `z`: 加速按 Zoom 级别查询

7. **内存预分配**
   - 批量缓冲区预分配内存
   - 减少动态内存分配开销

#### 性能提升

| 数据量 | 处理方式 | 内存峰值 | 写入时间 | 性能 |
|--------|----------|----------|----------|------|
| 10 万条 | 单条插入 | 500MB | ~10 分钟 | 基准 |
| 10 万条 | 批量写入 | 100MB | ~2 分钟 | 提升 5 倍 |
| 100 万条 | 批量 +TLS | 500MB | ~20 分钟 | 提升 10 倍 |
| 300 万条 | 批量 +TLS | 500MB | ~1 小时 | 稳定处理 |

**主要成就**：
- 批量写入减少数据库往返次数
- TLS 架构支持高并发写入
- 支持处理百万级瓦片而不会内存溢出
- 写入时间线性增长，无性能断点

## 安装

### 前提条件

- MongoDB C++ 驱动 (mongocxx)
- MongoDB 服务器 (4.0+ 推荐)
- 兼容 C++17 的编译器

### 构建说明

1. **安装 MongoDB C++ 驱动**：
   - Ubuntu/Debian: 从源码编译或使用包管理器
   - macOS: `brew install mongocxx`
   - 参考：https://mongocxx.org/mongocxx-v3/installation/

2. **构建 Tippecanoe**：
   ```bash
   make -j
   make install
   ```

## 使用方法

### 命令行选项

#### 单行配置（推荐）

```bash
# 标准格式（7 部分，严格要求）
tippecanoe-db --mongo "host:port:dbname:user:password:auth_source:collection" ...
```

**重要**：连接字符串必须严格包含 7 个部分，用冒号分隔。程序会自动在写入前清空集合。

#### 完整示例

```bash
# 基本用法
tippecanoe-db --mongo "localhost:27017:test:test:test:admin:china" -z14 -Z5

# 指定集合名
tippecanoe-db --mongo "localhost:27017:gis:user:pass:admin:tiles" -z14 -Z5

# 结合 PostGIS 和 MongoDB
tippecanoe-db \
  --postgis "localhost:5432:TippTest:postgres:pg:buildings:geom" \
  --mongo "localhost:27017:test:test:test:admin:china" \
  -o output.mbtiles \
  -z14 -Z5
```

### 选项详情

| 选项 | 描述 | 默认值 |
|--------|-------------|---------|
| `--mongo` | MongoDB 连接字符串，格式：host:port:dbname:user:password:auth_source:collection（必须 7 部分） | N/A |
| `--mongo-batch-size` | 每批次写入的瓦片数量 | 100 |
| `--mongo-pool-size` | 连接池大小 | 10 |
| `--mongo-timeout-ms` | 超时时间（毫秒） | 30000 |
| `--mongo-max-retries` | 最大重试次数 | 3 |
| `--mongo-write-concern` | 写确认级别 (0/1/2) | 1 |
| `--mongo-journal` | 启用日志 | false |
| `--mongo-no-indexes` | 不创建索引 | false |
| `--mongo-drop-collection` | 写入前清空集合（已自动启用） | true |
| `--mongo-no-progress` | 禁用进度报告 | false |

**注意**：使用 `--mongo` 单行配置时，会自动启用 `drop_collection_before_write`，在每次写入前清空集合。

### 连接字符串格式详解

#### 7 部分格式（标准格式，严格要求）
```
host:port:dbname:user:password:auth_source:collection
```
示例：`localhost:27017:gis:user:pass:admin:china`
- `host`: MongoDB 主机名
- `port`: MongoDB 端口号
- `dbname`: 数据库名称
- `user`: 用户名
- `password`: 密码
- `auth_source`: 认证源数据库（通常是 admin）
- `collection`: 集合名称

**重要**：
- 必须严格提供 7 个部分，不能多也不能少
- 所有字段都不能为空
- 程序会自动在写入前清空集合（无需 drop 标志）

## 高级配置

### 性能调优

#### 1. 调整批量大小

```bash
# 小数据集（< 10 万条）
tippecanoe-db --mongo "..." --mongo-batch-size 50

# 中等数据集（10-100 万条）
tippecanoe-db --mongo "..." --mongo-batch-size 100

# 大数据集（> 100 万条）
tippecanoe-db --mongo "..." --mongo-batch-size 200
```

**建议**：
- 批次越大，写入越快，但内存占用越高
- 最大批次大小：1000
- 最小批次大小：10

#### 2. 调整连接池

```bash
# 高并发场景
tippecanoe-db --mongo "..." --mongo-pool-size 30

# 低资源环境
tippecanoe-db --mongo "..." --mongo-pool-size 5
```

**建议**：
- 连接池大小 = CPU 核心数 * 2
- 最大连接池大小：50（防止资源耗尽）

#### 3. 写确认级别

```bash
# 最快（但不安全）
tippecanoe-db --mongo "..." --mongo-write-concern 0

# 平衡（默认）
tippecanoe-db --mongo "..." --mongo-write-concern 1

# 最安全
tippecanoe-db --mongo "..." --mongo-write-concern 2
```

**写确认级别说明**：
- **0 (NONE)**: 不等待确认，最快但可能丢失数据
- **1 (PRIMARY)**: 等待主节点确认（推荐）
- **2 (MAJORITY)**: 等待多数节点确认，最安全但最慢

#### 4. 超时和重试

```bash
# 高延迟网络
tippecanoe-db --mongo "..." --mongo-timeout-ms 60000 --mongo-max-retries 5

# 低延迟网络
tippecanoe-db --mongo "..." --mongo-timeout-ms 10000 --mongo-max-retries 2
```

### 大数据集处理最佳实践

#### 1. 自动清空集合

```bash
# 程序会自动在写入前清空集合，无需额外参数
tippecanoe-db \
  --mongo "localhost:27017:gis:user:pass:admin:china" \
  -z14 -Z5
```

**注意**：使用 `--mongo` 单行配置时，程序会自动在每次写入前清空集合，确保数据一致性。

#### 2. 结合 PostGIS 使用

```bash
# 从 PostGIS 读取，同时输出到 MBTiles 和 MongoDB
tippecanoe-db \
  --postgis=127.0.0.1:5432:TippTest:postgres:pg:china:geom \
  --mongo "localhost:27017:test:test:test:test:china" \
  -o output.mbtiles \
  -z14 -Z5
```

#### 3. 监控进度

```bash
# 启用进度报告（默认启用）
tippecanoe-db --mongo "..." 

# 禁用进度报告（减少日志输出）
tippecanoe-db --mongo "..." --mongo-no-progress
```

### 索引管理

#### 自动创建的索引

1. **唯一索引** `tile_coords_unique`
   ```javascript
   {z: 1, x: 1, y: 1}
   ```
   - 防止重复瓦片
   - 确保数据一致性

2. **查询索引** `zoom_level`
   ```javascript
   {z: 1}
   ```
   - 加速按 Zoom 级别查询
   - 优化范围查询

#### 禁用索引创建

```bash
# 如果索引已存在，可以跳过创建
tippecanoe-db --mongo "..." --mongo-no-indexes
```

## 使用场景

### 1. Web 地图瓦片服务

MongoDB 作为瓦片存储后端，提供：
- 高可用性（副本集）
- 水平扩展（分片）
- 灵活查询

```bash
# 生产环境配置
tippecanoe-db \
  --mongo "mongodb://user:pass@host1:27017,host2:27017,host3:27017/gis?replicaSet=rs0" \
  --mongo-batch-size 200 \
  --mongo-pool-size 30 \
  --mongo-write-concern 2 \
  -z14 -Z5
```

####### 2. 开发和测试

```bash
# 本地开发环境（自动清空集合）
tippecanoe-db \
  --mongo "localhost:27017:test:test:test:admin:dev_tiles" \
  -z12 -Z5
```

### 3. 数据备份和迁移

```bash
# 从 MBTiles 迁移到 MongoDB
tippecanoe-db \
  input.mbtiles \
  --mongo "localhost:27017:backup:user:pass:backup:tiles" \
  -z14 -Z5
```

## 监控和日志

### 进度报告

处理过程中会显示：
```
MongoDB 连接已初始化：test.china (连接池：10, 写确认：1)
MongoDB: 已写入 10000 个瓦片，共 100 个批次
MongoDB: 已写入 20000 个瓦片，共 200 个批次
...
MongoDB: 已写入 14624 个瓦片，共 147 个批次
```

### 错误重试

数据库操作失败时会显示：
```
MongoDB 写入失败（第 1/3 次尝试）: connection reset
MongoDB 写入失败（第 2/3 次尝试）: connection reset
MongoDB 重新连接成功
```

### 统计信息

处理完成后显示：
```
MongoDB: 已写入 14624 个瓦片，共 147 个批次
MongoDB: 重试 2 次，错误 2 次
```

## 故障排除

### 连接问题

- 确保 MongoDB 服务器正在运行
- 验证主机、端口、用户名和密码
- 检查防火墙设置
- 确认认证源正确

```bash
# 测试连接
mongosh "mongodb://user:pass@host:port/dbname?authSource=admin"
```

### 写入问题

- 检查集合权限
- 验证批量大小是否合理
- 增加超时时间
- 检查磁盘空间

### 性能问题

- 增加批量大小
- 增加连接池大小
- 使用更快的写确认级别（0 或 1）
- 确保索引已创建
- 使用 SSD 存储

### 内存问题

- 减少批量大小
- 减少连接池大小
- 减少工作线程数

### 构建问题

- 确保安装了 mongocxx
- 检查编译器兼容性 (需要 C++17)
- 验证 Makefile 具有正确的 MongoDB 包含路径

## 限制

1. **几何类型** - 当前支持点、线串和多边形几何类型
2. **依赖** - 需要安装 MongoDB C++ 驱动
3. **单写入者** - 当前实现不支持分布式并发写入同一集合
4. **分片支持** - 未测试分片集群环境

## 未来增强

1. ✅ ~~支持 XYZ 坐标系（与 mbtiles 兼容）~~ 已实现
2. ✅ ~~批量写入优化~~ 已实现
3. ✅ ~~线程安全（TLS 架构）~~ 已实现
4. ✅ ~~自动索引创建~~ 已实现
5. ✅ ~~错误重试机制~~ 已实现
6. ✅ ~~写确认配置~~ 已实现
7. ✅ ~~连接池管理~~ 已实现
8. ✅ ~~集合清空选项~~ 已实现（自动启用）
9. ✅ ~~严格连接字符串格式（7 部分）~~ 已实现
10. 支持分片集群（未来）
11. 支持增量更新（未来）
12. 支持瓦片元数据（未来）
13. 支持 GridFS 存储大瓦片（未来）

## 性能基准测试

### 测试环境
- MongoDB: 5.0 (单机)
- 硬件：Intel Core i7, 16GB RAM, SSD
- 数据集：中国行政边界数据（约 340 万条瓦片）

### 测试结果

| 数据量 | 批量大小 | 连接池 | 写入时间 | 内存峰值 | 吞吐量 |
|--------|----------|--------|----------|----------|--------|
| 10 万条 | 100 | 10 | ~2 分钟 | 100MB | 833 瓦片/秒 |
| 100 万条 | 100 | 10 | ~20 分钟 | 500MB | 833 瓦片/秒 |
| 340 万条 | 100 | 10 | ~1 小时 | 500MB | 944 瓦片/秒 |

**注意**：实际性能取决于网络延迟、磁盘 I/O 和系统配置。

### 优化建议

- **小数据集** (< 10 万条): 批量大小 50-100
- **中等数据集** (10-100 万条): 批量大小 100-200
- **大数据集** (> 100 万条): 批量大小 200，连接池 20-30

## 与 MBTiles 对比

| 特性 | MBTiles | MongoDB |
|------|---------|---------|
| 存储格式 | SQLite 文件 | 数据库集合 |
| 坐标系 | XYZ | XYZ（兼容） |
| 并发读写 | 有限 | 高并发 |
| 扩展性 | 单文件 | 支持副本集和分片 |
| 查询灵活性 | SQL | 丰富查询 |
| 备份 | 文件复制 | 数据库工具 |
| 适用场景 | 本地/离线 | Web 服务/在线 |

## 总结

Tippecanoe 的 MongoDB 支持经过全面优化，现在能够高效、稳定地将矢量瓦片存储到 MongoDB 数据库中。通过批量写入、线程本地存储和智能错误处理，系统可以处理百万级瓦片而不会内存溢出，同时保持合理的处理速度和资源消耗。MongoDB 作为瓦片存储后端，特别适合需要高可用性、水平扩展和灵活查询的 Web 地图应用场景。
