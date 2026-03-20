# tippecanoe-db 使用说明

## 概述

`tippecanoe-db` 是 tippecanoe 项目的专用版本，用于从 **PostGIS 数据库**读取地理空间数据并生成 **MongoDB 矢量瓦片**。

## 主要特性

### 1. 数据输入
- **仅支持 PostGIS 输入**（强制要求）
- 支持表查询和自定义 SQL 查询
- 支持多种几何类型（点、线、面）

### 2. 数据输出
- **强制 MongoDB 输出**（主要输出目标）
- 可选 MBTiles 文件输出（通过 `-o` 参数）
- 支持双写模式（同时输出到 MongoDB 和文件）

### 3. 性能优化
- **批量写入**：默认每 100 个瓦片批量插入
- **连接池**：可配置连接池大小（默认 10）
- **多线程**：并行处理瓦片生成
- **线程安全**：每个线程独立的 MongoDB 客户端

### 4. 数据可靠性
- **写确认级别**：可配置 w:0/w:1/w:majority
- **自动重试**：网络错误自动重试（默认 3 次）
- **异常处理**：完善的错误处理和日志记录
- **索引管理**：自动创建唯一索引防止重复

## 安装依赖

### MongoDB C++ Driver
```bash
# 系统已提供 mongo 开发库 /usr/local 下
# 包含 libmongocxx 和 libbsoncxx
```

### 编译
```bash
make tippecanoe-db
```

## 命令行参数

### PostGIS 输入参数

#### 单行配置（简化版）
```bash
--postgis "host:port:dbname:user:password[:table:geometry_field]"
```

**注意**：单行配置最多支持 7 个部分（host:port:dbname:user:password:table:geometry_field），**不支持 condition**。如需 WHERE 条件，请使用 `--postgis-sql` 参数。

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

**可选参数（7 个）**：
```bash
--postgis-table <table>              # 表名（与 geometry-field 一起使用时自动生成 SQL）
--postgis-geometry-field <geom>      # 几何字段名 (默认：geometry)
--postgis-sql <query>                # 自定义 SQL 查询（推荐，可包含 WHERE 条件）
--postgis-batch-size <N>             # 每批次处理要素数 (默认：1000)
--postgis-max-memory-mb <MB>         # 最大内存使用 MB (默认：512)
```

**WHERE 条件实现方式**：
```bash
# ✅ 推荐：使用 --postgis-sql
--postgis-sql "SELECT id, name, ST_AsGeoJSON(geom) as geojson FROM buildings WHERE height > 50"

# ❌ 不支持：单行配置中的 condition 部分会被忽略
```

### MongoDB 输出参数

#### 单行配置（推荐）
```bash
--mongo "host:port:dbname:user:password[:collection]"
```

**格式说明**：
- `host`: MongoDB 主机名或 IP 地址（必需）
- `port`: MongoDB 端口号（必需，默认 27017）
- `dbname`: 数据库名称（必需）
- `user`: 用户名（必需）
- `password`: 密码（必需）
- `collection`: Collection 名称（可选，默认 tiles）

**示例**：
```bash
--mongo "localhost:27017:gis:admin:secret:tiles"
--mongo "192.168.1.100:27017:mydb:user123:pass456:mycollection"
```

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

**其他配置**（2 个）：
```bash
--mongo-no-indexes            # 不自动创建索引（手动预创建时使用）
```

### 其他参数

```bash
# 输出选项
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

### 1. 基本用法
```bash
./tippecanoe-db \
  --mongo "localhost:27017:gis:admin:secret:tiles" \
  --postgis "localhost:5432:gis:postgres:secret:buildings:geom" \
  -z 14
```

### 2. 自定义 SQL 查询
```bash
./tippecanoe-db \
  --mongo "localhost:27017:gis:admin:secret:tiles" \
  --postgis-host localhost \
  --postgis-port 5432 \
  --postgis-dbname gis \
  --postgis-user postgres \
  --postgis-password secret \
  --postgis-sql "SELECT id, name, geom FROM buildings WHERE height > 10" \
  --postgis-geometry-field geom \
  -z 16
```

### 3. 高性能配置
```bash
./tippecanoe-db \
  --mongo "localhost:27017:gis:admin:secret:tiles" \
  --mongo-batch-size 200 \
  --mongo-pool-size 20 \
  --mongo-write-concern=1 \
  --postgis "localhost:5432:gis:postgres:secret:roads:geom" \
  -z 14 \
  --cluster-maxzoom 12
```

### 4. 高安全配置（金融数据）
```bash
./tippecanoe-db \
  --mongo "localhost:27017:gis:admin:secret:tiles" \
  --mongo-write-concern=2 \
  --mongo-journal \
  --mongo-wtimeout=5000 \
  --postgis "localhost:5432:gis:postgres:secret:parcels:geom" \
  -z 18
```

### 5. 双写模式（MongoDB + 文件）
```bash
./tippecanoe-db \
  --mongo "localhost:27017:gis:admin:secret:tiles" \
  -o output.mbtiles \
  --postgis "localhost:5432:gis:postgres:secret:landuse:geom" \
  -z 14
```

### 6. 禁用自动索引
```bash
./tippecanoe-db \
  --mongo "localhost:27017:gis:admin:secret:tiles" \
  --mongo-no-indexes \
  --postgis "localhost:5432:gis:postgres:secret:points:geom" \
  -z 12
```

## MongoDB 数据模型

### Document 结构
```javascript
{
  _id: ObjectId,           // MongoDB 自动生成
  x: <int>,                // 瓦片 X 坐标
  y: <int>,                // 瓦片 Y 坐标（TMS 翻转）
  z: <int>,                // 缩放级别
  d: <Binary>              // PBF 瓦片数据
}
```

### 索引策略

#### 自动创建的索引
1. **tile_coords_unique** (唯一索引)
   ```javascript
   { z: 1, x: 1, y: 1 }
   ```
   - 防止重复瓦片写入
   - 错误码：11000（重复键）

2. **zoom_level** (普通索引)
   ```javascript
   { z: 1 }
   ```
   - 加速按缩放级别查询

#### 手动创建索引（可选）
```javascript
// 范围查询优化
db.tiles.createIndex({ z: 1, x: 1, y: 1 })

// 统计查询优化
db.tiles.createIndex({ z: 1, x: 1 })
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
- 程序会自动跳过
- 如需覆盖，先清空集合

#### 4. PostGIS 连接失败
```
Connection to database failed: FATAL: password authentication failed
```
**解决**：
- 检查 PostgreSQL 密码
- 确认 pg_hba.conf 配置
- 验证用户权限

### 重试机制
- 默认重试次数：3 次
- 重试间隔：100ms × 重试次数
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
  --mongo "localhost:27017:gis:admin:secret:tiles" \
  --mongo-no-indexes \
  --mongo-write-concern=1 \
  --mongo-pool-size=20 \
  --postgis "localhost:5432:gis:postgres:secret:buildings:geom" \
  -z 14
```

### 2. 批量处理
```bash
# 使用脚本批量处理多个表
for table in buildings roads landuse; do
  ./tippecanoe-db \
    --mongo "localhost:27017:gis:admin:secret:tiles" \
    --postgis "localhost:5432:gis:postgres:secret:${table}:geom" \
    -z 14
done
```

### 3. 增量更新
```bash
# 先删除旧数据
mongosh
> use gis
> db.tiles.deleteMany({z: {$gte: 10, $lte: 14}})

# 重新生成
./tippecanoe-db \
  --mongo "localhost:27017:gis:admin:secret:tiles" \
  --postgis "localhost:5432:gis:postgres:secret:updated_buildings:geom" \
  -z 14
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

- `maindb.cpp` - 主程序源代码
- `mongo.hpp` - MongoDB 写入器头文件
- `mongo.cpp` - MongoDB 写入器实现
- `postgis.hpp` - PostGIS 读取器头文件
- `postgis.cpp` - PostGIS 读取器实现
- `Makefile` - 构建配置

## 相关文档

- [MONGO_OUTPUT.md](MONGO_OUTPUT.md) - MongoDB 输出功能详细说明
- [postgis.md](postgis.md) - PostGIS 参数配置说明
- [MONGO_EVALUATION.md](MONGO_EVALUATION.md) - 实现评估报告

## 版本信息

- **基于版本**: tippecanoe 最新源码
- **MongoDB Driver**: mongocxx v3.x
- **C++ 标准**: C++17
- **编译日期**: 见构建输出

## 许可证

与原 tippecanoe 项目保持一致。

## 联系方式

如有问题，请参考：
- MongoDB C++ Driver 文档：https://mongocxx.org/
- Tippecanoe 原始文档：https://github.com/felt/tippecanoe
- PostGIS 文档：https://postgis.net/documentation/
