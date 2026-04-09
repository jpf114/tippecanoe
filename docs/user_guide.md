# tippecanoe-db 详细说明文档

> 版本：1.0  
> 最后更新：2026-04-09  
> 适用范围：tippecanoe-db 定制化切片工具

---

## 1. 功能概述

### 1.1 工具简介

tippecanoe-db 是一款专为 PostGIS → MongoDB 场景设计的矢量切片工具，能够从 PostgreSQL/PostGIS 数据库读取空间数据，生成符合 Mapbox Vector Tile (MVT) 规范的矢量瓦片，并存储到 MongoDB 数据库中。

### 1.2 核心能力

| 能力 | 说明 |
|------|------|
| PostGIS 数据读取 | 自动检测 SRID 并转换到 WGS84，支持表名和自定义 SQL 两种输入方式 |
| WKB 二进制解析 | 直接解析 PostGIS 的 WKB/EWKB 几何数据，性能优于 WKT 文本格式 |
| 默认并行处理 | 基于 ctid 哈希分片的多线程并行读取，无需指定主键 |
| MongoDB 批量写入 | 支持批量 insert/upsert，自动创建索引，内置背压机制 |
| 元数据管理 | 可选写入瓦片集元数据到 MongoDB，包含 bounds、center、图层描述等 |
| 错误容错 | 解析/写入错误记录到 SQLite 日志，不中断主流程 |
| MBTiles 同步输出 | 同时生成 MBTiles 文件，便于对比验证 |

### 1.3 与原版 tippecanoe 的关系

tippecanoe-db 基于 tippecanoe 二次开发，共享瓦片生成核心引擎（排序、简化、编码），但数据输入输出层完全定制化。两个工具独立编译，互不影响。

---

## 2. 环境准备

### 2.1 系统要求

| 组件 | 最低版本 | 推荐版本 |
|------|---------|---------|
| 操作系统 | Linux (glibc 2.17+) | Ubuntu 20.04+ |
| GCC | 7.0 (C++17) | 11.0+ |
| PostgreSQL | 9.0+ | 14+ |
| PostGIS | 2.0+ | 3.0+ |
| MongoDB | 4.0+ | 6.0+ |
| mongocxx 驱动 | 3.6+ | 3.8+ |

### 2.2 依赖安装

**Ubuntu/Debian：**

```bash
# PostgreSQL 客户端库
sudo apt install libpq-dev postgresql-client

# MongoDB C++ 驱动（需手动编译安装）
# 参考: https://mongocxx.org/
git clone https://github.com/mongodb/mongo-cxx-driver.git --branch releases/stable
cd mongo-cxx-driver/build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build . -j$(nproc)
sudo cmake --install .

# SQLite3
sudo apt install libsqlite3-dev

# zlib
sudo apt install zlib1g-dev
```

### 2.3 编译

```bash
cd tippecanoe

# Debug 构建
make tippecanoe-db

# Release 构建（推荐生产环境）
BUILDTYPE=Release make tippecanoe-db

# 编译产物
ls -la tippecanoe-db
```

---

## 3. 操作流程指南

### 3.1 基本使用

#### 最简命令

```bash
tippecanoe-db \
  --postgis "localhost:5432:mydb:postgres:password:mytable:geom" \
  --mongo "localhost:27017:tilerender:admin:password:admin:mytiles" \
  -z10 -Z5
```

#### 完整参数示例

```bash
tippecanoe-db \
  --postgis "localhost:5432:mydb:postgres:password:mytable:geom" \
  --mongo "localhost:27017:tilerender:admin:password:admin:mytiles" \
  --mongo-drop-collection \
  --mongo-metadata \
  --mongo-batch-size 500 \
  -o output.mbtiles \
  -z10 -Z5 \
  --drop-densest-as-needed \
  --extend-zooms-if-still-dropping
```

### 3.2 参数详解

#### 3.2.1 PostGIS 输入参数

**方式一：连接字符串（推荐）**

```bash
--postgis "host:port:dbname:user:password:table:geometry_field"
```

| 部分 | 说明 | 示例 |
|------|------|------|
| host | 数据库主机 | localhost |
| port | 数据库端口 | 5432 |
| dbname | 数据库名 | TippTest |
| user | 用户名 | postgres |
| password | 密码 | mypassword |
| table | 表名 | china |
| geometry_field | 几何列名 | geom |

**方式二：独立参数**

```bash
--postgis-host localhost \
--postgis-port 5432 \
--postgis-database mydb \
--postgis-user postgres \
--postgis-password mypassword \
--postgis-table china \
--postgis-geometry-field geom
```

**方式三：自定义 SQL**

```bash
--postgis-host localhost \
--postgis-database mydb \
--postgis-user postgres \
--postgis-password mypassword \
--postgis-sql "SELECT geom, name, population FROM cities WHERE population > 100000"
```

> **注意：** 使用自定义 SQL 时，SQL 中的几何列将自动被 `ST_AsBinary()` 包裹，无需手动转换。

#### 3.2.2 MongoDB 输出参数

**方式一：连接字符串（推荐）**

```bash
--mongo "host:port:dbname:user:password:auth_source:collection"
```

| 部分 | 说明 | 示例 |
|------|------|------|
| host | MongoDB 主机 | localhost |
| port | MongoDB 端口 | 27017 |
| dbname | 数据库名 | test |
| user | 用户名 | admin |
| password | 密码 | admin |
| auth_source | 认证数据库 | admin |
| collection | 集合名 | china |

**方式二：独立参数**

```bash
--mongo-host localhost \
--mongo-port 27017 \
--mongo-database test \
--mongo-user admin \
--mongo-password admin \
--mongo-auth-source admin \
--mongo-collection china
```

**MongoDB 高级参数：**

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--mongo-batch-size` | 100 | 批量写入大小（10-1000） |
| `--mongo-drop-collection` | 否 | 写入前删除集合，使用 insert 模式（性能更高） |
| `--mongo-no-indexes` | 否 | 不创建索引 |
| `--mongo-metadata` | 否 | 写入元数据到 `{collection}_metadata` 集合 |

> **注意：** MongoDB 存储的瓦片数据为 gzip 压缩格式，与 MBTiles 格式一致。读取时需先解压。

#### 3.2.3 瓦片生成参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-z` / `--maximum-zoom` | 14 | 最大缩放级别 |
| `-Z` / `--minimum-zoom` | 0 | 最小缩放级别 |
| `-o` / `--output` | 无 | 输出 MBTiles 文件路径（可选） |
| `--drop-densest-as-needed` | 否 | 自动丢弃密集特征以控制瓦片大小 |
| `--extend-zooms-if-still-dropping` | 否 | 如仍需丢弃则扩展 zoom 级别 |
| `-x` / `--exclude` | 无 | 排除指定属性字段 |
| `-y` / `--include` | 无 | 仅包含指定属性字段 |
| `-j` / `--layer` | 表名 | 图层名称 |

### 3.3 典型操作流程

#### 场景一：全量导入（首次）

```bash
tippecanoe-db \
  --postgis "localhost:5432:mydb:postgres:pass:china:geom" \
  --mongo "localhost:27017:tilerender:admin:pass:admin:china" \
  --mongo-drop-collection \
  --mongo-metadata \
  -o china.mbtiles \
  -z10 -Z5
```

**说明：**
- `--mongo-drop-collection`：首次导入时删除旧数据，使用 insert 模式（性能最优）
- `--mongo-metadata`：写入元数据，方便读取端获取瓦片集信息
- `-o china.mbtiles`：同时生成 MBTiles 文件，用于验证

#### 场景二：增量更新（部分 zoom 级别）

```bash
tippecanoe-db \
  --postgis "localhost:5432:mydb:postgres:pass:china:geom" \
  --mongo "localhost:27017:tilerender:admin:pass:admin:china" \
  -z10 -Z10
```

**说明：**
- 不使用 `--mongo-drop-collection`，保留其他 zoom 级别的数据
- 使用 upsert 模式，自动覆盖相同坐标的瓦片
- 仅更新 z=10 级别

#### 场景三：自定义 SQL 查询

```bash
tippecanoe-db \
  --postgis-host localhost \
  --postgis-database mydb \
  --postgis-user postgres \
  --postgis-password pass \
  --postgis-sql "SELECT geom, name, pop FROM cities WHERE pop > 500000" \
  --mongo "localhost:27017:tilerender:admin:pass:admin:cities_large" \
  --mongo-drop-collection \
  --mongo-metadata \
  -z8 -Z3 \
  -j cities
```

**说明：**
- 使用 `--postgis-sql` 指定自定义查询，仅筛选人口大于 50 万的城市
- `-j cities` 指定图层名称

---

## 4. 使用场景说明

### 4.1 全国行政区划瓦片服务

**场景描述：** 将全国省市区三级行政区划数据从 PostGIS 导出为矢量瓦片，存储到 MongoDB，供 Web 地图服务调用。

```bash
tippecanoe-db \
  --postgis "pg-host:5432:gis_data:reader:pass:admin_regions:geometry" \
  --mongo "mongo-host:27017:tile_service:writer:pass:admin:admin_regions" \
  --mongo-drop-collection \
  --mongo-metadata \
  -z12 -Z3 \
  --drop-densest-as-needed
```

**MongoDB 读取端示例（Node.js）：**

```javascript
const { MongoClient } = require('mongodb');
const zlib = require('zlib');

async function getTile(z, x, y) {
  const client = new MongoClient('mongodb://writer:pass@mongo-host:27017/tile_service?authSource=admin');
  await client.connect();
  const db = client.db('tile_service');
  const tile = await db.collection('admin_regions').findOne({ z, x, y });
  await client.close();

  if (!tile || !tile.d) return null;

  // 瓦片数据为 gzip 压缩格式，需解压
  return zlib.gunzipSync(tile.d.buffer);
}

async function getMetadata() {
  const client = new MongoClient('mongodb://writer:pass@mongo-host:27017/tile_service?authSource=admin');
  await client.connect();
  const db = client.db('tile_service');
  const meta = await db.collection('admin_regions_metadata').findOne();
  await client.close();
  return JSON.parse(meta.metadata);
}
```

### 4.2 多图层瓦片服务

**场景描述：** 同一数据库中多个表分别生成瓦片，存储到 MongoDB 的不同集合。

```bash
# 图层1：道路
tippecanoe-db \
  --postgis "pg-host:5432:gis:reader:pass:roads:geom" \
  --mongo "mongo-host:27017:tiles:writer:pass:admin:roads" \
  --mongo-drop-collection --mongo-metadata \
  -z14 -Z5 -j roads

# 图层2：建筑
tippecanoe-db \
  --postgis "pg-host:5432:gis:reader:pass:buildings:geom" \
  --mongo "mongo-host:27017:tiles:writer:pass:admin:buildings" \
  --mongo-drop-collection --mongo-metadata \
  -z16 -Z10 -j buildings

# 图层3：水系
tippecanoe-db \
  --postgis "pg-host:5432:gis:reader:pass:waterways:geom" \
  --mongo "mongo-host:27017:tiles:writer:pass:admin:waterways" \
  --mongo-drop-collection --mongo-metadata \
  -z12 -Z4 -j waterways
```

### 4.3 数据更新场景

**场景描述：** 源数据更新后，仅重新生成受影响的 zoom 级别。

```bash
# 仅重新生成 z=8-10 级别
tippecanoe-db \
  --postgis "pg-host:5432:gis:reader:pass:china:geom" \
  --mongo "mongo-host:27017:tiles:writer:pass:admin:china" \
  -z10 -Z8
```

> **注意：** 不使用 `--mongo-drop-collection`，保留 z=3-7 的瓦片。upsert 模式会自动覆盖 z=8-10 的旧瓦片。

---

## 5. 输出数据说明

### 5.1 MongoDB 瓦片集合

**集合名：** 由 `--mongo-collection` 参数指定（如 `china`）

**文档结构：**

```json
{
  "_id": ObjectId("..."),
  "z": 10,
  "x": 825,
  "y": 402,
  "d": Binary(gzip_compressed_pbf_data)
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| z | int32 | 缩放级别 |
| x | int32 | X 坐标（XYZ 方案） |
| y | int32 | Y 坐标（XYZ 方案） |
| d | Binary | 瓦片数据（gzip 压缩的 PBF，与 MBTiles 格式一致） |

> **坐标系说明：** MongoDB 中使用 XYZ 坐标方案（Y 轴从上向下递增），与 TMS 方案（MBTiles 使用）的转换关系为：`xyz_y = 2^z - 1 - tms_y`
> 
> **数据格式说明：** MongoDB 存储的瓦片数据为 gzip 压缩格式，与 MBTiles 规范一致。读取时需先 gunzip 解压得到原始 PBF 数据。

**索引：**

| 索引名 | 类型 | 字段 |
|--------|------|------|
| tile_coords_unique | 唯一索引 | (z, x, y) |
| zoom_level | 普通索引 | (z) |

### 5.2 MongoDB 元数据集合

**集合名：** `{collection}_metadata`（如 `china_metadata`）

**文档结构：**

```json
{
  "_id": ObjectId("..."),
  "metadata": "{\"name\":\"china\",\"bounds\":[73.5,3.4,135.1,53.6],...}",
  "collection": "china",
  "timestamp": 1775724846722
}
```

**metadata JSON 字段详解：**

| 字段 | 类型 | 说明 |
|------|------|------|
| name | string | 图层名称 |
| description | string | 图层描述 |
| version | int | 数据版本 |
| type | string | 数据类型（通常为 "overlay"） |
| format | string | 格式（"pbf"） |
| minzoom | int | 最小缩放级别 |
| maxzoom | int | 最大缩放级别 |
| bounds | [4]double | 地理范围 [minlon, minlat, maxlon, maxlat] |
| center | [3]double | 中心点 [lon, lat, z] |
| attribution | string | 版权声明（如有） |
| generator | string | 生成工具信息 |
| vector_layers | array | 矢量图层描述及属性 schema |
| tilestats | object | 属性统计信息 |

### 5.3 MBTiles 文件（可选）

当指定 `-o` 参数时，同时生成标准 MBTiles 文件，可用于：
- 与 MongoDB 输出对比验证
- 作为离线备份
- 供不支持 MongoDB 的工具使用

### 5.4 SQLite 错误日志

**文件名：** `tile_errors.db`（位于可执行文件同目录下）

**查询示例：**

```bash
# 查看 PostGIS 解析错误
sqlite3 tile_errors.db "SELECT * FROM postgis_errors ORDER BY id DESC LIMIT 10;"

# 查看 MongoDB 写入错误
sqlite3 tile_errors.db "SELECT * FROM mongo_errors WHERE z=10 ORDER BY id DESC LIMIT 10;"

# 统计错误数量
sqlite3 tile_errors.db "SELECT 'postgis' AS source, count(*) FROM postgis_errors UNION ALL SELECT 'mongo', count(*) FROM mongo_errors UNION ALL SELECT 'general', count(*) FROM general_errors;"

# 清空错误日志
sqlite3 tile_errors.db "DELETE FROM postgis_errors; DELETE FROM mongo_errors; DELETE FROM general_errors;"
```

---

## 6. 常见问题解答

### 6.1 连接问题

**Q: 提示 "Connection to PostGIS failed"**

A: 检查以下几点：
1. PostgreSQL 服务是否运行：`pg_isready -h localhost -p 5432`
2. 用户名/密码是否正确：`psql -h localhost -U postgres -d mydb -c "SELECT 1"`
3. 防火墙是否放行 5432 端口
4. `pg_hba.conf` 是否允许密码认证

**Q: 提示 "MongoDB connection initialization failed"**

A: 检查以下几点：
1. MongoDB 服务是否运行：`mongosh --eval "db.runCommand({ping:1})"`
2. 用户名/密码/认证数据库是否正确
3. MongoDB 是否允许远程连接（`bindIp` 配置）
4. mongocxx 驱动版本是否兼容

### 6.2 数据问题

**Q: 提示 "WKB geometry field not found in query result"**

A: 可能原因：
1. 几何列名不正确，检查 `--postgis-geometry-field` 参数
2. 使用自定义 SQL 时，SQL 中包含了同名的 `wkb` 列
3. 表中没有几何数据

**Q: 瓦片数量比预期少**

A: 可能原因：
1. 源数据 SRID 不是 4326，且自动转换失败 — 检查 SRID：`SELECT ST_SRID(geom) FROM table LIMIT 1`
2. 几何数据为 NULL 或空 — 检查：`SELECT count(*) FROM table WHERE geom IS NULL`
3. 瓦片大小超限被丢弃 — 添加 `--drop-densest-as-needed` 参数

**Q: MongoDB 中瓦片数据与 MBTiles 不一致**

A: 检查坐标系转换：
- MBTiles 使用 TMS 坐标系（Y 轴从下向上）
- MongoDB 使用 XYZ 坐标系（Y 轴从上向下）
- 转换公式：`xyz_y = 2^z - 1 - tms_y`

### 6.3 性能问题

**Q: 处理速度慢**

A: 优化建议：
1. 确认使用了并行读取（默认启用，线程数等于 CPU 核心数）
2. 增大 `--mongo-batch-size`（默认 100，可调至 500-1000）
3. 使用 `--mongo-drop-collection`（insert 模式比 upsert 快 2-3 倍）
4. 检查 PostgreSQL 和 MongoDB 的网络延迟

**Q: 内存占用过高**

A: 调整建议：
1. 减小 PostGIS 批量大小（通过 `config.hpp` 中的 `DEFAULT_POSTGIS_BATCH_SIZE`）
2. 减小 MongoDB 批量大小（`--mongo-batch-size`）
3. 检查是否有超大几何体导致内存飙升

**Q: 并行读取提示 "ctid sharding not supported"**

A: 这是正常的 fallback 行为，发生在：
1. 表是分区表
2. 使用了自定义 SQL 子查询
3. PostgreSQL 版本不支持 ctid

此时自动降级为单线程读取，不影响数据正确性。

### 6.4 错误日志问题

**Q: tile_errors.db 文件在哪里？**

A: 位于 tippecanoe-db 可执行文件的同目录下。如果通过绝对路径运行，则在该路径的目录下。

**Q: 程序退出码含义**

| 退出码 | 常量 | 含义 |
|--------|------|------|
| 0 | EXIT_OK | 正常退出 |
| 1 | EXIT_OPEN | 文件/数据库打开失败 |
| 2 | EXIT_PBF | Protobuf 编码错误 |
| 17 | EXIT_MONGO | MongoDB 写入错误 |
| 其他 | — | tippecanoe 原版错误码 |

---

## 7. 注意事项

### 7.1 数据安全

1. **密码安全：** `--postgis-password` 和 `--mongo-password` 通过命令行传递，可被 `ps aux` 看到。生产环境建议：
   - 使用环境变量：`export PGPASSWORD=xxx`
   - 使用 `.pgpass` 文件
   - 限制数据库用户权限为只读

2. **SQL 注入：** 表名和几何列名已通过 `PQescapeIdentifier()` 转义，但 `--postgis-sql` 中的自定义 SQL 由用户自行负责安全性。

3. **MongoDB 权限：** 建议为 tippecanoe-db 创建专用的 MongoDB 用户，仅授予 `readWrite` 权限。

### 7.2 数据一致性

1. **SRID 要求：** 所有几何数据必须具有有效的 SRID。如果 SRID 为 0 或未定义，工具会尝试使用 `ST_Transform(geom, 4326)` 转换，但可能失败。

2. **游标事务：** 游标模式使用 `REPEATABLE READ` 事务隔离，确保读取期间数据一致性。但非游标模式下读取期间源数据可能变化。

3. **增量更新：** 不使用 `--mongo-drop-collection` 时，upsert 模式按 (z, x, y) 匹配覆盖。如果源数据范围缩小，旧瓦片不会被自动删除。

### 7.3 性能建议

1. **全量导入优先使用 `--mongo-drop-collection`：** insert 模式比 upsert 快 2-3 倍。

2. **批量大小调优：**
   - 小数据集（<1万瓦片）：`--mongo-batch-size 100`（默认）
   - 中等数据集（1-50万瓦片）：`--mongo-batch-size 500`
   - 大数据集（>50万瓦片）：`--mongo-batch-size 1000`

3. **PostgreSQL 端优化：**
   - 确保几何列有 GIST 索引：`CREATE INDEX idx_geom ON table USING GIST(geom);`
   - 增加 `work_mem`：`SET work_mem = '256MB';`
   - 大表考虑 `VACUUM ANALYZE` 更新统计信息

4. **MongoDB 端优化：**
   - 确保 WiredTiger 缓存足够：`storage.wiredTiger.engineConfig.cacheSizeGB`
   - 写入期间可临时关闭 journal：`--mongo-write-concern none`（牺牲安全性换性能）
   - 写入完成后执行 `db.collection.compact()` 回收空间

### 7.4 已知限制

| 限制 | 说明 | 规避方案 |
|------|------|---------|
| GEOMETRYCOLLECTION 不支持 | WKB 解析器可解析但无法映射到瓦片几何类型 | 在 SQL 中用 `ST_Dump()` 展开 |
| 分区表不支持并行 | ctid 在分区表上不可用 | 自动降级为单线程 |
| 自定义 SQL 不支持并行 | 子查询结果无 ctid | 自动降级为单线程 |
| 最大 zoom 级别 22 | 受 VLA 修复后的 vector 分配限制 | 实际使用中很少超过 z=18 |
| 单集合单图层 | 一个 MongoDB 集合只能存储一个图层 | 多图层使用不同集合名 |

---

## 8. 维护建议

### 8.1 日常运维

**定期清理错误日志：**

```bash
sqlite3 tile_errors.db "DELETE FROM postgis_errors WHERE timestamp < datetime('now', '-30 days');"
sqlite3 tile_errors.db "DELETE FROM mongo_errors WHERE timestamp < datetime('now', '-30 days');"
sqlite3 tile_errors.db "VACUUM;"
```

**MongoDB 索引维护：**

```bash
mongosh --eval "
  db.china.getIndexes();
  db.china.stats();
  db.china_metadata.stats();
" tilerender
```

**数据验证脚本：**

```bash
#!/bin/bash
# verify.sh - 验证 MongoDB 与 MBTiles 数据一致性

MBTILES=$1
DB=$2
COLL=$3

echo "=== Tile count comparison ==="
mb_count=$(sqlite3 "$MBTILES" "SELECT count(*) FROM tiles;")
mg_count=$(mongosh --quiet --eval "db.${COLL}.countDocuments({})" "$DB")
echo "MBTiles: $mb_count, MongoDB: $mg_count"

if [ "$mb_count" = "$mg_count" ]; then
    echo "✅ Tile counts match"
else
    echo "❌ Tile counts differ"
fi

echo ""
echo "=== Per-zoom comparison ==="
for z in $(sqlite3 "$MBTILES" "SELECT DISTINCT zoom_level FROM tiles ORDER BY zoom_level;"); do
    mb_z=$(sqlite3 "$MBTILES" "SELECT count(*) FROM tiles WHERE zoom_level=$z;")
    mg_z=$(mongosh --quiet --eval "db.${COLL}.countDocuments({z:$z})" "$DB")
    match=$([ "$mb_z" = "$mg_z" ] && echo "✅" || echo "❌")
    echo "z=$z: MBTiles=$mb_z MongoDB=$mg_z $match"
done
```

### 8.2 版本升级

1. **mongocxx 驱动升级：** 升级后需检查头文件路径是否变化，更新 Makefile 中的 `-I` 路径
2. **PostgreSQL 升级：** 通常向后兼容，但需验证 `hashtext()` 行为是否变化
3. **tippecanoe 上游合并：** maindb.cpp 中从 main.cpp 复制的代码需手动合并上游变更

### 8.3 监控指标

| 指标 | 获取方式 | 告警阈值 |
|------|---------|---------|
| MongoDB 写入错误率 | `tile_errors.db` mongo_errors 表 | > 0.1% |
| PostGIS 解析错误率 | `tile_errors.db` postgis_errors 表 | > 1% |
| MongoDB 写入延迟 | `db.china.stats().writeLatency` | > 100ms |
| MongoDB 集合大小 | `db.china.stats().size` | 取决于磁盘 |
| MongoDB 索引碎片率 | `db.china.stats().indexDetails` | > 30% |

### 8.4 故障排查流程

```
tippecanoe-db 运行异常
    │
    ├── 退出码 != 0
    │     ├── EXIT_OPEN (1): 检查 PostGIS 连接
    │     ├── EXIT_MONGO (17): 检查 MongoDB 连接和写入
    │     └── 其他: 查看 stderr 输出
    │
    ├── 瓦片数量异常
    │     ├── 检查 tile_errors.db 中的解析错误
    │     ├── 检查源数据 SRID 和几何有效性
    │     └── 对比 MBTiles 文件（如有）
    │
    ├── 性能异常
    │     ├── 检查是否降级为单线程（ctid 不支持）
    │     ├── 检查 MongoDB 写入延迟
    │     └── 检查 PostgreSQL 查询计划
    │
    └── 数据不一致
          ├── 检查坐标系转换（TMS vs XYZ）
          ├── 检查 MongoDB 索引是否正确
          └── 使用 verify.sh 脚本对比
```

---

## 9. 参数速查表

### PostGIS 参数

| 参数 | 短选项 | 默认值 | 说明 |
|------|--------|--------|------|
| `--postgis` | — | — | 连接字符串（7部分冒号分隔） |
| `--postgis-host` | — | localhost | 数据库主机 |
| `--postgis-port` | — | 5432 | 数据库端口 |
| `--postgis-database` | — | — | 数据库名 |
| `--postgis-user` | — | — | 用户名 |
| `--postgis-password` | — | — | 密码 |
| `--postgis-table` | — | — | 表名 |
| `--postgis-geometry-field` | — | geometry | 几何列名 |
| `--postgis-sql` | — | — | 自定义 SQL |

### MongoDB 参数

| 参数 | 短选项 | 默认值 | 说明 |
|------|--------|--------|------|
| `--mongo` | — | — | 连接字符串（7部分冒号分隔） |
| `--mongo-host` | — | — | MongoDB 主机 |
| `--mongo-port` | — | — | MongoDB 端口 |
| `--mongo-database` | — | — | 数据库名 |
| `--mongo-user` | — | — | 用户名 |
| `--mongo-password` | — | — | 密码 |
| `--mongo-auth-source` | — | — | 认证数据库 |
| `--mongo-collection` | — | — | 集合名 |
| `--mongo-batch-size` | — | 100 | 批量写入大小 (10-1000) |
| `--mongo-drop-collection` | — | 否 | 写入前删除集合 |
| `--mongo-no-indexes` | — | 否 | 不创建索引 |
| `--mongo-metadata` | — | 否 | 写入元数据 |

### 瓦片生成参数（常用）

| 参数 | 短选项 | 默认值 | 说明 |
|------|--------|--------|------|
| `--maximum-zoom` | `-z` | 14 | 最大缩放级别 |
| `--minimum-zoom` | `-Z` | 0 | 最小缩放级别 |
| `--output` | `-o` | — | 输出 MBTiles 文件 |
| `--layer` | `-j` | 表名 | 图层名称 |
| `--exclude` | `-x` | — | 排除属性字段 |
| `--include` | `-y` | — | 仅包含属性字段 |
| `--drop-densest-as-needed` | — | 否 | 自动丢弃密集特征 |
| `--extend-zooms-if-still-dropping` | — | 否 | 扩展 zoom 级别 |
| `--quiet` | `-q` | 否 | 静默模式 |
