# tippecanoe-db 使用说明

## 概述

`tippecanoe-db` 是 tippecanoe 项目的专用版本，用于从 **PostGIS 数据库**读取地理空间数据并输出到 **MongoDB 数据库**和/或 **MBTiles 文件**。

### 主要特性
- **单一输入源**: 仅支持 PostGIS 数据库输入（强制要求）
- **多目标输出**: 支持 MongoDB（强制）、MBTiles 文件（可选）
- **高性能**: 批量写入、线程池、连接池优化
- **高可靠**: 自动重试、写确认、异常安全
- **坐标兼容**: MongoDB 使用 XYZ 坐标系，与 MBTiles 完全兼容
- **自动清空集合**: 每次写入前自动清空 MongoDB 集合，确保数据一致性

## 输入输出模式

### 输入（强制）
- **PostGIS 数据库**: 唯一的输入源
  - 支持表查询和自定义 SQL 查询
  - 支持多种几何类型：点、线串、多边形、几何集合
  - 通过 libpq 库直接连接 PostgreSQL/PostGIS

### 输出（强制 + 可选）
- **MongoDB**（强制）: 主要输出目标
  - 批量写入，线程安全
  - 自动索引管理
  - 支持副本集和分片
- **MBTiles**（可选）: 通过 `-o` 参数指定
  - SQLite 文件格式
  - 与 MongoDB 输出完全兼容
- **PMTiles**（可选）: 通过 `-e` 参数指定目录
  - 云优化瓦片格式

## 安装依赖

### MongoDB C++ Driver
```bash
# 系统已提供 mongo 开发库 /usr/local 下
# 包含 libmongocxx 和 libbsoncxx
```

### PostgreSQL 客户端库
```bash
# Ubuntu/Debian
sudo apt-get install libpq-dev

# CentOS/RHEL
sudo yum install postgresql-devel

# macOS
brew install postgresql
```

### 编译
```bash
make tippecanoe-db
```

## 命令行参数

### PostGIS 输入参数（必需）

#### 单行配置（简化版）
```bash
--postgis "host:port:dbname:user:password[:table:geometry_field]"
```

**注意**：单行配置最多支持 7 个部分（host:port:dbname:user:password:table:geometry_field）。如需 WHERE 条件，请使用 `--postgis-sql` 参数。

**示例**：
```bash
--postgis "localhost:5432:gis:postgres:secret"
--postgis "localhost:5432:gis:postgres:secret:buildings:geom"
```

#### 单独参数

**必需参数（5 个）**：
```bash
--postgis-host <host>         # PostgreSQL 主机 (默认：localhost)
--postgis-port <port>         # PostgreSQL 端口 (默认：5432)
--postgis-dbname <dbname>     # 数据库名称（必需）
--postgis-user <user>         # 用户名（必需）
--postgis-password <pwd>      # 密码（必需）
```

**可选参数（3 个）**：
```bash
--postgis-table <table>              # 表名（与 geometry-field 一起使用时自动生成 SQL）
--postgis-geometry-field <geom>      # 几何字段名 (默认：geometry)
--postgis-sql <query>                # 自定义 SQL 查询（推荐，可包含 WHERE 条件）
```

**WHERE 条件实现方式**：
```bash
# ✅ 推荐：使用 --postgis-sql
--postgis-sql "SELECT id, name, ST_AsGeoJSON(geom) as geojson FROM buildings WHERE height > 50"

# ❌ 不支持：单行配置中的 condition 部分
```

### MongoDB 输出参数（必需）

#### 单行配置（严格格式）
```bash
--mongo "host:port:dbname:user:password:auth_source:collection"
```

**注意**：必须严格提供 7 个部分，用冒号分隔。程序会自动在写入前清空集合。

**格式说明**（必须 7 部分）：
- `host`: MongoDB 主机名或 IP 地址（必需）
- `port`: MongoDB 端口号（必需，默认 27017）
- `dbname`: 数据库名称（必需）
- `user`: 用户名（必需）
- `password`: 密码（必需）
- `auth_source`: 认证源数据库（必需）
- `collection`: Collection 名称（必需）

**示例**：
```bash
# 标准 7 部分格式
--mongo "localhost:27017:gis:user:pass:admin:china"

# 测试环境
--mongo "localhost:27017:test:test:test:admin:test_tiles"

# 生产环境
--mongo "mongodb.example.com:27017:production:admin:secret:admin:production_tiles"
```

**错误示例**（会立即退出并报错）：
```bash
# ❌ 5 部分：缺少 auth_source 和 collection
--mongo "localhost:27017:test:test:test:test"

# ❌ 6 部分：缺少 collection
--mongo "localhost:27017:test:test:test:test:admin"

# ❌ 8 部分：多余的 drop 标志
--mongo "localhost:27017:test:test:test:test:admin:china:drop"
```

**重要特性**：
- 使用 `--mongo` 参数时，程序会**自动清空集合**后再写入新数据
- 无需手动指定 `--mongo-drop-collection` 参数
- 确保每次运行都是干净的数据集

#### 单独参数

**必需参数**（使用单行配置时只需 `--mongo`，使用单独参数时需要以下 3 个）：
```bash
--mongo-dbname <name>         # 数据库名称（必需）
--mongo-username <user>       # 用户名（必需）
--mongo-password <pwd>        # 密码（必需）
```

**可选参数**（12 个）：

**连接配置**（4 个）：
```bash
--mongo-host <host>           # MongoDB 主机 (默认：localhost)
--mongo-port <port>           # MongoDB 端口 (默认：27017)
--mongo-collection <name>     # Collection 名称 (默认：tiles)
--mongo-auth-source <db>      # 认证数据库 (默认：admin)
```

**性能配置**（3 个）：
```bash
--mongo-batch-size <N>        # 批量插入大小 (默认：100, 范围：10-1000)
--mongo-pool-size <N>         # 连接池大小 (默认：10)
--mongo-timeout <ms>          # 超时时间 ms (默认：30000)
```

**写确认配置**（3 个）：
```bash
--mongo-write-concern <0|1|2> # 写确认级别 (默认：1)
                              # 0: w:0 无确认（最快，测试环境）
                              # 1: w:1 主节点确认（默认，生产环境推荐）
                              # 2: w:majority 多数节点确认（最安全，金融数据）
--mongo-journal               # 启用 journal 日志（配合 write-concern=2 使用）
--mongo-wtimeout <ms>         # 写确认超时 ms (默认：5000)
```

**可选参数**（2 个）：
```bash
--mongo-no-indexes            # 不自动创建索引（手动预创建时使用）
--mongo-drop-collection       # 写入前清空集合（已自动启用，无需手动指定）
```

**注意**：使用 `--mongo` 单行配置时，`drop_collection_before_write` 会自动设置为 `true`，因此通常不需要手动指定 `--mongo-drop-collection` 参数。

### 其他参数

```bash
# 输出选项（可选）
-o <file.mbtiles>             # 同时输出到 MBTiles 文件
-e <directory>                # 同时输出到目录结构

# 缩放级别
-z <zoom>                     # 最大缩放级别
-Z <zoom>                     # 最小缩放级别
--cluster-maxzoom <zoom>      # 聚合最大缩放级别

# 特征抽取
--drop-rate <rate>            # 抽取率 (默认：根据缩放级别自动)
--gamma <level>               # Gamma 参数

# 投影
--projection <proj>           # 投影设置 (默认：EPSG:3857)

# 通用选项
-q                          # 安静模式（减少输出）
-v                          # 详细模式（增加输出）
--help                      # 显示帮助信息
```

## 使用示例

### 1. 基本用法（最小配置）
```bash
./tippecanoe-db \
  --postgis "localhost:5432:gis:postgres:secret:buildings:geom" \
  --mongo "localhost:27017:gis:postgres:secret:admin:tiles" \
  -z 14
```

**注意**：MongoDB 连接字符串必须包含 7 个部分：host:port:dbname:user:password:auth_source:collection

### 2. 自定义 SQL 查询
```bash
./tippecanoe-db \
  --postgis-host localhost \
  --postgis-port 5432 \
  --postgis-dbname gis \
  --postgis-user postgres \
  --postgis-password secret \
  --postgis-sql "SELECT id, name, ST_AsGeoJSON(geom) as geojson FROM buildings WHERE height > 10" \
  --postgis-geometry-field geom \
  --mongo "localhost:27017:gis:postgres:secret:admin:tiles" \
  -z 16
```

### 3. 高性能配置
```bash
./tippecanoe-db \
  --postgis "localhost:5432:gis:postgres:secret:roads:geom" \
  --mongo "localhost:27017:gis:postgres:secret:admin:tiles" \
  --mongo-batch-size 200 \
  --mongo-pool-size 20 \
  --mongo-write-concern=1 \
  -z 14 \
  --cluster-maxzoom 12
```

### 4. 高安全配置（金融数据）
```bash
./tippecanoe-db \
  --postgis "localhost:5432:gis:postgres:secret:parcels:geom" \
  --mongo "localhost:27017:gis:postgres:secret:admin:tiles" \
  --mongo-write-concern=2 \
  --mongo-journal \
  --mongo-wtimeout=5000 \
  -z 18
```

### 5. 双写模式（MongoDB + MBTiles）
```bash
./tippecanoe-db \
  --postgis "localhost:5432:gis:postgres:secret:landuse:geom" \
  --mongo "localhost:27017:gis:postgres:secret:admin:tiles" \
  -o output.mbtiles \
  -z 14
```

### 6. 自动清空集合（无需额外参数）
```bash
./tippecanoe-db \
  --postgis "localhost:5432:gis:postgres:secret:points:geom" \
  --mongo "localhost:27017:gis:admin:secret:tiles" \
  -z 12
```

**注意**：使用 `--mongo` 参数时，程序会自动在写入前清空集合，无需在连接字符串中添加 `:drop` 标志。

### 7. 禁用自动索引
```bash
./tippecanoe-db \
  --postgis "localhost:5432:gis:postgres:secret:points:geom" \
  --mongo "localhost:27017:gis:postgres:secret:admin:tiles" \
  --mongo-no-indexes \
  -z 12
```

## MongoDB 数据模型

### Document 结构
```javascript
{
  _id: ObjectId,           // MongoDB 自动生成
  x: <int>,                // 瓦片 X 坐标（XYZ 坐标系）
  y: <int>,                // 瓦片 Y 坐标（XYZ 坐标系，不翻转）
  z: <int>,                // 缩放级别
  d: <Binary>              // PBF 瓦片数据
}
```

**重要**: 坐标系统使用 **XYZ（Google/OSM）**，与 MBTiles 完全兼容，**不做 Y 坐标翻转**。

### 索引策略

#### 自动创建的索引
1. **tile_coords_unique** (唯一索引)
   ```javascript
   { z: 1, x: 1, y: 1 }
   ```
   - 防止重复瓦片写入
   - 错误码：11000（重复键）
   - 确保数据一致性

2. **zoom_level** (普通索引)
   ```javascript
   { z: 1 }
   ```
   - 加速按缩放级别查询
   - 优化范围查询

#### 手动创建索引（可选）
```javascript
// 范围查询优化
db.tiles.createIndex({ z: 1, x: 1, y: 1 })

// 统计查询优化
db.tiles.createIndex({ z: 1, x: 1 })
```

#### 禁用自动索引
```bash
# 如果索引已存在，可以跳过创建
./tippecanoe-db --mongo "..." --mongo-no-indexes ...
```

## 性能调优

### 1. 批量大小调整
- **低延迟网络** (< 10ms): `--mongo-batch-size 50-100`
- **中等延迟** (10-50ms): `--mongo-batch-size 100-200`
- **高延迟** (> 50ms): `--mongo-batch-size 200-500`

### 2. 连接池配置
- **默认**: `--mongo-pool-size 10`
- **多核 CPU**: `--mongo-pool-size <CPU_cores * 2>`
- **最大建议**: 50

### 3. 写确认级别选择
- **测试环境**: `--mongo-write-concern=0` (最快)
- **生产环境**: `--mongo-write-concern=1` (默认，平衡)
- **关键数据**: `--mongo-write-concern=2` (最安全)

### 4. 内存使用
- 每个线程独立缓冲区：`threads × batch_size × avg_tile_size`
- 默认配置约占用：8 × 100 × 10KB ≈ 8MB

## 错误处理

### 常见错误及解决方案

#### 1. MongoDB 认证失败
```
Error: Failed to initialize MongoDB connection: Authentication failed.
```
**解决**：
- 检查用户名密码是否正确
- 确认认证数据库 (`--mongo-auth-source`)
- 验证用户权限

#### 2. MongoDB 连接失败
```
Warning: Failed to create MongoDB indexes: Connection refused
```
**解决**：
- 检查 MongoDB 服务是否运行
- 确认主机和端口正确
- 检查防火墙设置

#### 3. 重复键错误
```
MongoDB flush_batch failed: E11000 duplicate key error
```
**解决**：
- 正常情况，瓦片已存在
- 程序会自动跳过（无序插入模式）
- 如需覆盖，使用 `--mongo-drop-collection` 清空集合

#### 4. PostGIS 连接失败
```
Connection to database failed: FATAL: password authentication failed
```
**解决**：
- 检查 PostgreSQL 密码
- 确认 pg_hba.conf 配置
- 验证用户权限

#### 5. PostGIS 配置缺失
```
Error: PostGIS configuration is required
Error: Either PostGIS table or SQL query is required
```
**解决**：
- 必须提供 `--postgis` 参数或单独的 PostGIS 参数
- 必须指定 `--postgis-table` 或 `--postgis-sql`

#### 6. MongoDB 输出缺失
```
Error: MongoDB output is required. Use --omongo to enable MongoDB output
```
**解决**：
- 必须提供 `--mongo` 参数或单独的 MongoDB 参数
- MongoDB 输出是强制要求的

### 重试机制
- 默认重试次数：3 次
- 重试间隔：100ms × 重试次数（指数退避）
- 最大等待时间：5 秒
- 失败后程序退出（EXIT_MONGO）

## 监控与日志

### 启动信息
```
MongoDB output enabled: gis.tiles (batch size: 100)
Created unique index on (z, x, y) for gis.tiles
Created index on z for gis.tiles
MongoDB connection initialized for gis.tiles (pool size: 10, write concern: 1)
Reading from PostGIS database: postgres@localhost:5432/gis
```

### 进度输出
```
Writing gis at zoom 14
MongoDB: Written 15000 tiles in 150 batches
MongoDB: Written 30000 tiles in 300 batches
```

### 完成统计
```
MongoDB: Written 45000 tiles in 450 batches
```

## 最佳实践

### 1. 生产环境部署
```bash
# 预创建索引
mongosh
> use gis
> db.tiles.createIndex({z: 1, x: 1, y: 1}, {unique: true})
> db.tiles.createIndex({z: 1})

# 运行程序
./tippecanoe-db \
  --postgis "localhost:5432:gis:postgres:secret:buildings:geom" \
  --mongo "localhost:27017:gis:postgres:secret:admin:tiles" \
  --mongo-no-indexes \
  --mongo-write-concern=1 \
  --mongo-pool-size=20 \
  -z 14
```

### 2. 增量更新
```bash
# 方法 1: 自动清空集合（默认行为）
./tippecanoe-db \
  --postgis "localhost:5432:gis:postgres:secret:updated_buildings:geom" \
  --mongo "localhost:27017:gis:admin:secret:tiles" \
  -z 14
```

**注意**：程序默认会在每次写入前清空集合，因此重新运行命令会自动替换旧数据。

```bash
# 方法 2: 手动删除旧数据
mongosh
> use gis
> db.tiles.deleteMany({z: {$gte: 10, $lte: 14}})

# 重新生成
./tippecanoe-db \
  --postgis "localhost:5432:gis:postgres:secret:updated_buildings:geom" \
  --mongo "localhost:27017:gis:admin:secret:tiles" \
  -z 14
```

### 3. 批量处理多个表
```bash
# 使用脚本批量处理多个表
for table in buildings roads landuse; do
  ./tippecanoe-db \
    --postgis "localhost:5432:gis:postgres:secret:${table}:geom" \
    --mongo "localhost:27017:gis:postgres:secret:admin:tiles" \
    -z 14
done
```

## 故障排查

### 1. 性能问题
```bash
# 启用详细日志
./tippecanoe-db -v ...

# 检查 MongoDB 慢查询
mongosh
> db.setProfilingLevel(2)
```

### 2. 内存问题
```bash
# 减少批量大小和连接池
./tippecanoe-db \
  --mongo-batch-size 50 \
  --mongo-pool-size 5 \
  ...
```

### 3. 网络问题
```bash
# 增加超时时间
./tippecanoe-db \
  --mongo-timeout 60000 \
  --mongo-wtimeout 10000 \
  ...
```

## 文件清单

- `maindb.cpp` - 主程序源代码，命令行参数解析，PostGIS 和 MongoDB 集成逻辑
- `mongo.hpp` - MongoDB 写入器头文件，定义 mongo_config 结构和 MongoWriter 类
- `mongo.cpp` - MongoDB 写入器实现，包含连接管理、批量写入、错误重试、索引创建等功能
- `postgis.hpp` - PostGIS 读取器头文件，定义 postgis_config 结构和 PostGISReader 类
- `postgis.cpp` - PostGIS 读取器实现，包含数据库连接、数据查询、流式处理、内存管理等功能
- `tile-db.hpp` - MongoDB 瓦片数据库接口
- `Makefile` - 构建配置

## 核心 API 参考

### PostGISReader 类（postgis.cpp）

#### 构造函数
```cpp
PostGISReader::PostGISReader(const postgis_config &cfg);
```
初始化 PostGIS 读取器，配置数据库连接参数和性能优化参数。

#### 主要方法
```cpp
bool PostGISReader::connect();
bool PostGISReader::read_features(std::vector<struct serialization_state> &sst, 
                                   size_t layer, const std::string &layername);
void PostGISReader::process_feature(PGresult *res, int row, int nfields, 
                                     int geom_field_index, ...);
void PostGISReader::process_batch(PGresult *res, ...);
bool PostGISReader::execute_query(const std::string &query);
bool PostGISReader::execute_query_with_retry(const std::string &query);
bool PostGISReader::check_memory_usage();
void PostGISReader::log_progress(size_t processed, size_t total, const char *stage);
std::string PostGISReader::escape_json_string(const char *value);
```

### MongoWriter 类（mongo.cpp）

#### 静态方法
```cpp
static MongoWriter* get_thread_local_instance(const mongo_config &cfg);
static void destroy_thread_local_instances();
static void initialize_global();
static size_t get_global_total_tiles();
static size_t get_global_total_batches();
static size_t get_global_total_retries();
static size_t get_global_total_errors();
```

#### 实例方法
```cpp
MongoWriter::MongoWriter(const mongo_config &cfg);
MongoWriter::~MongoWriter() noexcept;

void MongoWriter::initialize_thread();
void MongoWriter::close() noexcept;

void MongoWriter::write_tile(int z, int x, int y, const char *data, size_t len);
void MongoWriter::flush_all() noexcept;
void MongoWriter::flush_batch();

void MongoWriter::reconnect();
void MongoWriter::create_indexes_if_needed();
void MongoWriter::erase_zoom(int z);
```

## 相关文档

- [mongo.md](mongo.md) - MongoDB 支持详细说明
- [postgis.md](postgis.md) - PostGIS 参数配置说明

## 版本信息

- **基于版本**: tippecanoe 最新源码
- **MongoDB Driver**: mongocxx v3.x
- **PostgreSQL Driver**: libpq
- **C++ 标准**: C++17
- **编译日期**: 见构建输出

## 许可证

与原 tippecanoe 项目保持一致。

## 联系方式

如有问题，请参考：
- MongoDB C++ Driver 文档：https://mongocxx.org/
- Tippecanoe 原始文档：https://github.com/felt/tippecanoe
- PostGIS 文档：https://postgis.net/documentation/
