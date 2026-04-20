# tippecanoe-db 详细设计文档

> 版本：2.0  
> 最后更新：2026-04-13  
> 适用范围：tippecanoe-db 定制化切片工具

---

## 1. 功能架构设计

### 1.1 系统定位

tippecanoe-db 是基于开源项目 tippecanoe 二次开发的定制化矢量切片工具，核心定位为：

**PostGIS 空间数据库 → 矢量瓦片生成 → MongoDB 存储**

与原版 tippecanoe 的关键差异：

| 维度 | tippecanoe（原版） | tippecanoe-db（定制版） |
|------|-------------------|----------------------|
| 数据输入 | GeoJSON/GeoBuf/FlatGeoBuf/CSV/PostGIS | 仅 PostGIS |
| 数据输出 | MBTiles/Directory/PMTiles | MongoDB + MBTiles |
| 并行策略 | 单线程或需 PK 字段 | 默认并行（range/key-hash 优先，ctid 兜底） |
| 几何格式 | WKT 文本 | WKB 二进制 |
| 错误处理 | exit() 终止 | SQLite 错误日志 + 容错继续 |
| 元数据 | MBTiles metadata 表 | MongoDB 元数据集合 |

### 1.2 整体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        tippecanoe-db                            │
│                                                                 │
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────────┐   │
│  │  PostGIS      │   │  Tiling      │   │  MongoDB          │   │
│  │  Reader       │──▶│  Engine      │──▶│  Writer           │   │
│  │              │   │  (继承自      │   │                  │   │
│  │  ┌──────────┐│   │   tippecanoe)│   │  ┌──────────────┐│   │
│  │  │WKB Parser││   │              │   │  │ Batch Buffer ││   │
│  │  └──────────┘│   │              │   │  └──────────────┘│   │
│  │  ┌──────────┐│   │              │   │  ┌──────────────┐│   │
│  │  │Parallel  ││   │              │   │  │ Upsert/Insert││   │
│  │  │Reader    ││   │              │   │  └──────────────┘│   │
│  │  └──────────┘│   │              │   │  ┌──────────────┐│   │
│  └──────────────┘   │              │   │  │ Metadata     ││   │
│                      │              │   │  │ Writer       ││   │
│  ┌──────────────┐   │              │   │  └──────────────┘│   │
│  │ Error Logger │   │              │   │  ┌──────────────┐│   │
│  │ (SQLite)     │   │              │   │  │ Backpressure ││   │
│  └──────────────┘   └──────────────┘   │  └──────────────┘│   │
│                                         └──────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

### 1.3 数据流总览

```
PostGIS Database                    tippecanoe-db                    MongoDB
┌──────────┐     ┌──────────────────────────────────┐     ┌──────────────┐
│          │     │                                  │     │              │
│  china   │────▶│ 1. ST_AsBinary() → WKB 二进制    │     │  china       │
│  table   │     │ 2. WKB Parser → drawvec 坐标     │     │  (tiles)     │
│          │     │ 3. serialize_feature() → 序列化  │────▶│              │
│  SRID    │     │ 4. radix sort → 排序合并          │     │  china_      │
│  4326    │     │ 5. write_tile() → 瓦片生成        │     │  metadata    │
│          │     │ 6. gzip compress → 压缩           │     │  (metadata)  │
└──────────┘     │ 7. MongoDB batch write            │     └──────────────┘
                 └──────────────────────────────────┘
                          │
                          ▼
                   ┌──────────────┐
                   │ tile_errors  │
                   │ .db (SQLite) │
                   └──────────────┘
```

---

## 2. 模块划分

### 2.1 模块依赖关系

```
maindb.cpp
  ├── postgis_manager.cpp ──┐
  │     └── postgis.cpp     │
  │           ├── wkb_parser.cpp
  │           └── error_logger.cpp
  │                          ├── mongo_manager.cpp
  │                          │     └── mongo.cpp
  └── tile-db.cpp ──────────┘
        └── error_logger.cpp
```

### 2.2 模块职责详述

#### 2.2.1 maindb.cpp — 主入口与管线编排

| 函数/区域 | 行范围 | 职责 |
|-----------|--------|------|
| 全局变量声明 | 72-128 | PostGIS/MongoDB 配置、运行时参数 |
| `read_input()` | 941-1170 | 数据读取管线：连接 PostGIS → 并行读取 → 序列化 → 排序 |
| `maindb()` | 2280-3258 | 参数解析、初始化、调用 read_input、清理 |

**关键流程：**

```
maindb()
  ├── 1. 参数解析（--postgis, --postgis-shard-key, --postgis-shard-mode, --mongo, --mongo-fail-on-discard 等）
  ├── 2. ErrorLogger::initialize() — 初始化 SQLite 错误日志
  ├── 3. MongoDB::initialize_global() — 初始化 mongocxx 全局实例
  ├── 4. read_input()
  │     ├── PostGIS::validate_config() — 验证 PostGIS 配置
  │     ├── PostGIS::ParallelReader::read_parallel() — 并行读取
  │     │     └── PostGISReader::read_features() — 各线程读取+解析
  │     ├── radix_sort/merge — 排序合并
  │     └── traverse_zooms → write_tile — 瓦片生成+写入
  ├── 5. MongoWriter::write_metadata() — 写入元数据（如启用）
  ├── 6. MongoDB::cleanup_global() — 清理 TLS 实例
  ├── 7. ErrorLogger::print_summary() — 打印错误汇总
  └── 8. 返回退出码
```

#### 2.2.2 postgis.cpp — PostGIS 数据读取

**类：`PostGISReader`**

| 方法 | 职责 |
|------|------|
| `connect()` | 通过 libpq 连接 PostgreSQL |
| `get_cached_srid()` | 静态方法，查询并缓存几何列 SRID |
| `build_select_query()` | 静态方法，构建 `ST_AsBinary()` 查询 |
| `read_features()` | 核心读取方法，支持游标/非游标模式 |
| `process_batch()` | 批量处理查询结果 |
| `process_feature()` | 单行处理：WKB 解析 → 属性提取 → 序列化 |
| `decode_bytea()` | PostgreSQL bytea hex 格式解码 |
| `check_memory_usage()` | 内存使用监控 |

**SRID 缓存机制：**

```
首次调用 get_cached_srid()
  ├── 执行 SRID 探测 SQL
  ├── 以连接维度 key（host|port|dbname|table|geometry|sql）缓存
  └── 后续同 key 调用直接返回缓存值（线程安全，mutex 保护）
```

**查询构建策略：**

```
SRID == 4326:
  SELECT ST_AsBinary(geom) AS wkb, <projection> FROM "table"

SRID != 4326:
  SELECT ST_AsBinary(ST_Transform(geom, 4326)) AS wkb, <projection> FROM "table"

自定义 SQL:
  SELECT ST_AsBinary((sql)::geometry) AS wkb, <projection> FROM (sql) AS _subq

其中 <projection> 默认是 `*`；当指定 `--postgis-columns` 时为列白名单投影。
```

#### 2.2.3 postgis_manager.cpp — 并行读取管理

**类：`PostGIS::ParallelReader`**

**并行策略：可配置分片（range/key-hash/ctid）**

```
线程 i (0 ≤ i < num_threads) 的查询条件:
  WHERE (abs(hashtext(ctid::text)) % num_threads) = i

ctid 不可用时的 fallback:
  1. 执行探测查询 SELECT 1 FROM (分片查询) AS _probe LIMIT 1
  2. 探测失败 → 回退到原始查询
  3. 仅主线程(thread_id=0)执行，其他线程退出
```

**线程模型：**

```
ParallelReader::read_parallel()
  ├── num_threads <= 1: 单线程读取
  └── num_threads > 1:
        ├── 创建 num_threads 个 std::thread
        ├── 每个线程创建独立的 PostGISReader 实例
        ├── 各线程独立连接数据库并执行分片查询
        ├── join() 等待所有线程完成
        └── 汇总 total_features 和 total_parse_errors
```

#### 2.2.4 wkb_parser.cpp — WKB 二进制解析器

**类：`WKBReader`**

**支持的 WKB 类型：**

| WKB 类型码 | 几何类型 | 映射到 |
|-----------|---------|--------|
| 1 | Point | GEOM_POINT → VT_POINT |
| 2 | LineString | GEOM_LINESTRING → VT_LINE |
| 3 | Polygon | GEOM_POLYGON → VT_POLYGON |
| 4 | MultiPoint | GEOM_MULTIPOINT → VT_POINT |
| 5 | MultiLineString | GEOM_MULTILINESTRING → VT_LINE |
| 6 | MultiPolygon | GEOM_MULTIPOLYGON → VT_POLYGON |
| 7 | GeometryCollection | GEOM_TYPES → 跳过（不支持的类型） |

**EWKB 标志位处理：**

```
type_int = read_uint32()
base_type = type_int & 0xFF
has_z    = (type_int & 0x80000000) != 0
has_m    = (type_int & 0x40000000) != 0
has_srid = (type_int & 0x20000000) != 0
```

**坐标转换流程：**

```
WKB 二进制数据
  → 读取字节序标记 (0=大端, 1=小端)
  → 读取类型码 + 标志位
  → 读取 SRID（如有）
  → 读取坐标对 (lon, lat)
  → projection->project(lon, lat, 32, &x, &y)  // 投影到 Web Mercator
  → draw(op, x, y)  // 存入 drawvec
```

**公共接口：**

```cpp
WKBResult parse_wkb(const uint8_t* data, size_t len);
WKBResult parse_wkb_hex(const std::string& hex);
```

#### 2.2.5 mongo.cpp — MongoDB 写入器

**类：`MongoWriter`**

**连接池架构（v2.0 重构）：**

```
全局共享:
  mongocxx::pool (连接池)
    ├── 由 get_or_create_pool() 创建
    ├── pool_created (atomic<bool>) 保护
    └── 多线程共享连接

线程局部 (TLS):
  thread_local MongoWriter*
    ├── batch_buffer (每线程独立)
    ├── batch_coords (每线程独立)
    └── 统计计数器 (atomic)

全局状态 (mutex 保护):
  erased_zooms (set<int>)
    ├── 记录已删除的 zoom 级别
    └── 用于选择性 upsert
```

**写入模式：**

| 模式 | 触发条件 | 实现方式 | 性能 |
|------|---------|---------|------|
| insert | zoom 未被 erase | `insert_many(ordered=false)` | 高（无需索引查找） |
| upsert | zoom 已被 erase | `bulk_write(replace_one, upsert=true)` | 中（需索引匹配） |

**批量写入流程（v2.0 重构）：**

```
write_tile(z, x, y, data, len)
  ├── 构建 BSON 文档: {z, x, y, d: Binary(gzip_compressed_data)}
  ├── 加入 batch_buffer
  └── buffer 满 (≥ batch_size) → flush_batch()

flush_batch()
  ├── get_erased_zooms_snapshot() — 一次加锁获取快照
  ├── 单次遍历分离 insert_buf / upsert_buf
  ├── flush_batch_with_retry(false, insert_buf) — insert 模式
  └── flush_batch_with_retry(true, upsert_buf) — upsert 模式

flush_batch_with_retry(upsert_mode, batch_buf, coords)
  ├── build_write_concern() — 构建写入关注级别
  ├── insert 模式: collection.insert_many(views, ordered=false)
  ├── upsert 模式: collection.create_bulk_write() + replace_one(upsert=true)
  ├── 成功: 更新统计计数器
  └── 失败: 重试 max_retries 次
      ├── 指数退避 + 随机抖动 (100ms → ... → 30000ms 上限)
      └── 持续失败 (≥2轮): 丢弃数据并记录 total_discarded_tiles
```

**线程安全设计（v2.0 重构）：**

```
全局:
  mongocxx::instance (单例, initialize_global())
  mongocxx::pool (连接池, mutex + atomic 保护)
  erased_zooms (set<int>, mutex 保护)
  std::once_flag (collection_drop_flag, index_create_flag)

线程局部:
  thread_local std::unique_ptr<MongoWriter> tls_mongo_writer
  每个工作线程独立的 batch_buffer、batch_coords

原子计数器:
  std::atomic<size_t> total_tiles_written (TLS 统计)
  std::atomic<size_t> total_discarded_tiles (丢弃统计)
  std::atomic<size_t> global_total_tiles (全局统计汇总)
```

**元数据写入（v2.0 重构）：**

```cpp
void write_metadata_bson(const struct metadata &meta);
// 写入到 {collection}_metadata 集合
// 文档结构: BSON 原生构建，包含完整字段：
//   - name, description, version, type, format
//   - minzoom, maxzoom, bounds, center
//   - vector_layers, tilestats (v2.0 新增)
//   - collection, timestamp
```

**索引创建：**

```
tile_coords_unique: 唯一索引 on (z, x, y)
zoom_level: 普通索引 on (z)
```

#### 2.2.6 error_logger.cpp — SQLite 错误日志

**类：`ErrorLogger`（单例模式）**

**数据库文件：** `{可执行文件路径}/tile_errors.db`

**表结构：**

```sql
CREATE TABLE postgis_errors (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT NOT NULL,
    row_num INTEGER,
    geometry_type TEXT,
    error_message TEXT NOT NULL,
    data_preview TEXT
);

CREATE TABLE mongo_errors (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT NOT NULL,
    z INTEGER,
    x INTEGER,
    y INTEGER,
    operation TEXT NOT NULL,
    error_message TEXT NOT NULL
);

CREATE TABLE general_errors (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT NOT NULL,
    source TEXT NOT NULL,
    error_message TEXT NOT NULL
);
```

**性能优化：**
- WAL 日志模式
- 批量事务提交（每 50 条 COMMIT）
- mutex 保护线程安全

---

## 3. 核心算法说明

### 3.1 WKB 解析算法

**时间复杂度：** O(N)，N 为坐标点数  
**空间复杂度：** O(N)，存储 drawvec

```
parse_wkb(data, len)
  ├── 读取字节序标记 (1 byte)
  ├── 读取类型码 (4 bytes)
  ├── 解析 EWKB 标志位
  ├── 读取 SRID (4 bytes, 如有)
  └── 根据 base_type 分派:
        ├── Point: 1 个坐标对
        ├── LineString: npoints 个坐标对
        ├── Polygon: nrings × npoints 个坐标对
        ├── Multi*: ngeoms × 递归 parse_geometry()
        └── GeometryCollection: ngeoms × 递归 parse_geometry()
```

### 3.2 ctid 哈希分片算法

**目标：** 将表中的行均匀分配到 N 个线程

```
分片条件: (abs(hashtext(ctid::text)) % N) = thread_id

ctid: PostgreSQL 系统列，格式为 (block_number, offset)
hashtext(): PostgreSQL 内置哈希函数，返回 int32
abs(): 取绝对值确保非负
% N: 取模分配到 N 个分片

特性:
  - 无需用户指定主键
  - 单次查询内 ctid 稳定
  - 分片均匀性依赖 hashtext 的分布特性
  - 不支持分区表和复杂子查询（有 fallback）
```

### 3.3 游标批处理算法

```
BEGIN ISOLATION LEVEL REPEATABLE READ;
DECLARE cursor_name SCROLL CURSOR FOR base_query;

LOOP:
  FETCH FORWARD batch_size FROM cursor_name;
  IF ntuples == 0: BREAK;
  process_batch(res);
END LOOP;

CLOSE cursor_name;
COMMIT;
```

**事务隔离：** REPEATABLE READ 确保游标读取期间数据一致性

### 3.4 MongoDB 批量写入算法

```
write_tile():
  pending_writes++
  if pending_writes >= MAX_PENDING_WRITES: sleep(10ms)
  buffer.push(doc)
  if buffer.size() >= batch_size: flush_batch()

flush_batch():
  for attempt in [1..max_retries]:
    try:
      if use_upsert: bulk_write(replace_one, upsert=true)
      else: insert_many(ordered=false)
      pending_writes -= buffer.size()
      return SUCCESS
    catch:
      log_error()
      if attempt < max_retries: reconnect(exponential_backoff)
      else if failure_rounds >= 3: discard_buffer()
```

### 3.5 排序合并算法（继承自 tippecanoe）

```
radix_sort():
  基于 geohash 前缀的基数排序
  时间复杂度: O(N × maxzoom / P), P 为并行度
  空间复杂度: O(N)

merge():
  多路归并已排序的临时文件
  使用 mmap 访问临时文件
```

### 3.6 字节级收敛策略（PostGIS 路径 vs 主线路径）

为实现 `tippecanoe-db` 与主线 `tippecanoe` 的字节级一致，最终采用了以下对齐策略：

1. **几何语义对齐：**  
   在 WKB Polygon 解析后补充 `VT_CLOSEPATH` 终止标记，使后续 `fix_polygon()` 的外环/内环状态机与 `read_json.cpp` 路径一致，避免 tile 集合差异。

2. **复杂属性文本对齐：**  
   对 PostgreSQL 返回的复杂文本（数组/对象）做 JSON 风格规范化（如 `{100000}` → `[100000]`、去除无意义空白），消除不同输入通道的文本表示差异。

3. **属性顺序对齐：**  
   `process_feature()` 中对属性键采用稳定顺序策略（优先键 + 其余键稳定排序），避免 tag 序列顺序漂移导致 payload hash 不一致。

4. **对比模式固定化：**  
   回归对比统一使用 `--postgis-shard-mode none`，排除并行读取调度造成的非业务顺序波动。

**最终验证结果（同源数据、同参数）：**

- tile 坐标集合一致
- `SUM(LENGTH(tile_data))` 一致
- `hex(tile_data)` 全量排序后 `sha256` 一致

这说明当前 PostGIS/WKB 输入路径在输出语义与序列化层面已与主线路径收敛到字节级等价。

---

## 4. 数据流程设计

### 4.1 主数据流

```
┌─────────────────────────────────────────────────────────────────────┐
│                         主数据流                                     │
│                                                                     │
│  PostGIS                                                            │
│  ┌─────────┐    WKB Binary     ┌──────────┐    drawvec             │
│  │ ST_As   │──────────────────▶│ WKB      │─────────────────┐      │
│  │ Binary()│    (bytea hex)    │ Parser   │  (投影坐标)      │      │
│  └─────────┘                   └──────────┘                  │      │
│                                                              ▼      │
│  ┌──────────┐    serial_feature    ┌──────────┐    sorted       │      │
│  │ serialize │──────────────────▶│ radix    │──────────▶  │      │
│  │ _feature()│                    │ sort     │    features    │      │
│  └──────────┘                    └──────────┘              │      │
│                                                              ▼      │
│  ┌──────────┐    gzip compressed    ┌──────────┐    BSON docs     │
│  │ write    │─────────────────────▶│ MongoDB  │──────────────┘     │
│  │ _tile()  │    (pbf data)        │ Writer   │                    │
│  └──────────┘                      └──────────┘                    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 4.2 错误数据流

```
PostGIS 读取失败 ─────┐
WKB 解析失败 ─────────┤
                       ├──▶ ErrorLogger::log_parse_error()
                       │     └──▶ SQLite: postgis_errors 表
MongoDB 写入失败 ─────┤
MongoDB flush 失败 ───┤
                       ├──▶ ErrorLogger::log_mongo_error()
                       │     └──▶ SQLite: mongo_errors 表
                       │
                       └──▶ ErrorLogger::log_error()
                             └──▶ SQLite: general_errors 表
```

### 4.3 MongoDB 文档结构

**瓦片集合（如 `china`）：**

```json
{
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
| y | int32 | Y 坐标（XYZ 方案，非 TMS） |
| d | Binary | 瓦片数据（gzip 压缩的 PBF，与 MBTiles 格式一致） |

> **注意：** MongoDB 存储的瓦片数据始终为 gzip 压缩格式，与 MBTiles 规范一致。读取端需先 gunzip 解压。

**元数据集合（如 `china_metadata`）：**

```json
{
  "_id": ObjectId("..."),
  "name": "china",
  "description": "",
  "version": 2,
  "type": "overlay",
  "format": "pbf",
  "minzoom": 5,
  "maxzoom": 10,
  "bounds": [73.502355, 3.397162, 135.09567, 53.563269],
  "center": [104.299013, 28.480216, 7],
  "vector_layers": "[{\"id\":\"china\",\"fields\":{\"name\":\"String\"},\"minzoom\":5,\"maxzoom\":10}]",
  "tilestats": "{\"layerCount\":1,\"layers\":[{\"layer\":\"china\",\"count\":14182,\"geometry\":\"Polygon\"}]}",
  "attribution": "",
  "generator": "tippecanoe-db v2.0",
  "collection": "china",
  "timestamp": 1775724846722
}
```

> **注意：** v2.0 版本起，元数据使用 BSON 原生构建，`vector_layers` 和 `tilestats` 字段以 JSON 字符串形式存储，读取端需先解析 JSON。

---

## 5. 接口定义规范

### 5.1 命令行接口

```
tippecanoe-db [OPTIONS]

PostGIS 输入参数:
  --postgis=host:port:dbname:user:password:table:geometry_field
      连接字符串格式，7 部分以冒号分隔
      示例: localhost:5432:mydb:postgres:pass:china:geom

  --postgis-host=HOST       数据库主机 (默认: localhost)
  --postgis-port=PORT       数据库端口 (默认: 5432)
  --postgis-dbname=DB       数据库名
  --postgis-user=USER       用户名
  --postgis-password=PASS   密码
  --postgis-table=TABLE     表名（与 --postgis-sql 二选一）
  --postgis-geometry-field=FIELD  几何列名 (默认: geometry)
  --postgis-sql=SQL         自定义 SQL 查询（与 --postgis-table 二选一）
  --postgis-columns=CSV     属性列白名单（如 id,name,level）
  --postgis-columns-best-effort  非严格列模式：非法/不存在列跳过并告警
  --postgis-canonical-attr-order    启用主线一致性属性顺序（默认开启）
  --postgis-no-canonical-attr-order 关闭主线一致性属性顺序（按键名通用排序）
  --postgis-profile       输出 PostGIS 读取性能摘要（默认关闭）
  --postgis-shard-key=COL   并行分片字段（建议整数主键）
  --postgis-shard-mode=MODE 分片模式: auto|range|key|none
  --postgis-progress-count  开启精确进度（执行 COUNT(*)，默认关闭）

MongoDB 输出参数:
  --mongo=host:port:dbname:user:password:auth_source:collection
      连接字符串格式，7 部分以冒号分隔
      示例: localhost:27017:test:admin:pass:admin:china

  --mongo-host=HOST         MongoDB 主机
  --mongo-port=PORT         MongoDB 端口
  --mongo-dbname=DB         数据库名
  --mongo-username=USER     用户名
  --mongo-password=PASS     密码
  --mongo-auth-source=DB    认证数据库
  --mongo-collection=NAME   集合名
  --mongo-pool-size=N       连接池大小 (默认: 10, 上限: 50)
  --mongo-timeout=MS        超时毫秒 (默认: 30000)
  --mongo-batch-size=N      批量写入大小 (默认: 100, 范围: 10-1000)
  --mongo-drop-collection   写入前删除集合 (使用 insert 模式)
  --mongo-no-indexes        不创建索引
  --mongo-metadata          写入元数据到 {collection}_metadata
  --mongo-fail-on-discard   瓦片丢弃视为失败退出（默认）
  --mongo-no-fail-on-discard 有丢弃仅告警，不失败退出

瓦片生成参数:
  -z/--maximum-zoom=Z       最大缩放级别 (默认: 14)
  -Z/--minimum-zoom=Z       最小缩放级别 (默认: 0)
  -o/--output=FILE          输出 MBTiles 文件路径
  ... (其他参数同原版 tippecanoe)
```

### 5.2 C++ 核心接口

```cpp
// WKB 解析
struct WKBResult {
    int geometry_type;    // GEOM_POINT, GEOM_LINESTRING, ...
    drawvec coordinates;  // 投影后的坐标向量
    bool valid;           // 解析是否成功
    std::string error;    // 错误信息
};
WKBResult parse_wkb(const uint8_t* data, size_t len);
WKBResult parse_wkb_hex(const std::string& hex);

// PostGIS 读取
class PostGISReader {
    bool connect();
    bool read_features(std::vector<serialization_state>& sst,
                       size_t layer, const std::string& layername,
                       size_t thread_id = 0, size_t num_threads = 1);
    static int get_cached_srid(const postgis_config& cfg, void* conn);
    static std::string build_select_query(const postgis_config& cfg, int srid);
};

// 并行读取
namespace PostGIS {
    class ParallelReader {
        bool read_parallel(std::vector<serialization_state>& sst,
                          size_t layer, const std::string& layername);
    };
}

// MongoDB 写入
class MongoWriter {
    static void initialize_global();
    static MongoWriter* get_writer_instance(const mongo_config& cfg);
    static void destroy_writer_instance();
    void write_tile(int z, int x, int y, const char* data, size_t len);
    void flush_all() noexcept;
    void write_metadata_bson(const struct metadata& meta);
    void erase_zoom(int z);
    bool should_use_upsert_for_zoom(int z) const;
    
    struct tile_coords { int z, x, y; };
};

// MongoDB 统计
struct GlobalStats {
    size_t total_tiles;
    size_t total_batches;
    size_t total_retries;
    size_t total_errors;
    size_t total_discarded;  // v2.0 新增
};
GlobalStats get_global_stats();

// 错误日志
class ErrorLogger {
    static ErrorLogger& instance();
    bool initialize(const std::string& exec_path);
    void log_parse_error(int row, const std::string& geometry_type,
                         const std::string& message, const std::string& preview);
    void log_mongo_error(int z, int x, int y,
                         const std::string& operation, const std::string& message);
    void print_summary(bool quiet_mode = false) const;
};
```

---

## 6. 技术选型依据

### 6.1 WKB 替代 WKT

| 维度 | WKT | WKB |
|------|-----|-----|
| 格式 | 文本 | 二进制 |
| 传输大小 | 大（坐标为十进制字符串） | 小（坐标为 8 字节 double） |
| 解析速度 | 慢（文本解析 + 字符串→浮点） | 快（直接内存读取） |
| 精度 | 可能丢失精度 | 完全保留 IEEE 754 精度 |
| PostgreSQL 函数 | `ST_AsText()` | `ST_AsBinary()` |

**选型结论：** WKB 在传输效率、解析速度、精度保留三方面均优于 WKT，实测读取性能提升 3-5 倍。

### 6.2 ctid 哈希分片替代 PK 范围分片

| 维度 | PK 范围分片 | ctid 哈希分片 |
|------|-----------|-------------|
| 前提条件 | 需要整数主键 | 无需任何前提 |
| 均匀性 | 依赖 PK 分布 | 依赖 hashtest 分布 |
| 配置复杂度 | 需指定 `--postgis-shard-key` | `--postgis-shard-mode auto` 自动回退 |
| 兼容性 | 所有表 | 不支持分区表/子查询 |

**选型结论：** ctid 分片零配置、通用性强，配合 fallback 机制兼顾兼容性。

### 6.3 SQLite 作为错误日志存储

| 维度 | 文件日志 | SQLite |
|------|---------|--------|
| 结构化查询 | 不支持 | SQL 查询 |
| 并发写入 | 需自行实现 | WAL 模式支持 |
| 事务保证 | 无 | ACID |
| 依赖 | 无 | 已有（-lsqlite3） |

**选型结论：** 项目已依赖 sqlite3（MBTiles），无额外依赖成本，且提供结构化查询能力。

### 6.4 MongoDB 写入模式选择

| 场景 | 模式 | 原因 |
|------|------|------|
| 全量导入 | insert (drop + insert) | 无需索引查找，性能最高 |
| 增量更新 | upsert (replace_one) | 避免重复键冲突 |

---

## 7. 系统交互逻辑

### 7.1 PostGIS 连接与查询交互

```
tippecanoe-db                          PostgreSQL
    │                                      │
    │──── PQconnectdbParams() ────────────▶│  建立连接
    │◀─── CONNECTION_OK ─────────────────│
    │                                      │
    │──── ST_SRID(geom) ─────────────────▶│  查询 SRID
    │◀─── 4326 ──────────────────────────│
    │                                      │
    │──── BEGIN ISOLATION LEVEL            │
    │     REPEATABLE READ ────────────────▶│  开启事务
    │──── DECLARE cursor ─────────────────▶│  声明游标
    │                                      │
    │──── FETCH FORWARD 1000 ─────────────▶│  批量获取
    │◀─── 1000 rows (WKB binary) ─────────│
    │──── ... (循环) ─────────────────────▶│
    │◀─── 0 rows (结束) ──────────────────│
    │                                      │
    │──── CLOSE cursor ───────────────────▶│  关闭游标
    │──── COMMIT ─────────────────────────▶│  提交事务
```

### 7.2 MongoDB 写入交互

```
tippecanoe-db                          MongoDB
    │                                      │
    │──── mongocxx::instance() ───────────▶│  全局初始化
    │                                      │
    │──── collection.drop() ──────────────▶│  删除集合(可选)
    │──── create_index(z,x,y) ────────────▶│  创建索引
    │──── create_index(z) ────────────────▶│
    │                                      │
    │──── insert_many([doc1..docN]) ──────▶│  批量写入
    │◀─── Acknowledged ──────────────────│
    │──── ... (循环) ─────────────────────▶│
    │                                      │
    │──── insert_one(metadata) ───────────▶│  写入元数据(可选)
    │◀─── Acknowledged ──────────────────│
```

### 7.3 错误处理交互

```
PostGIS/MongoDB 操作
    │
    ├── 成功 → 继续处理
    │
    └── 失败
         ├── PostGIS 解析失败
         │     └── ErrorLogger::log_parse_error()
         │           └── SQLite INSERT INTO postgis_errors
         │
         ├── MongoDB 写入失败
         │     ├── 重试 (max_retries 次)
         │     ├── 重连 (指数退避)
         │     ├── 记录 ErrorLogger::log_mongo_error()
         │     └── 持续失败 → 丢弃数据 (≥3轮)
         │
         └── MongoDB 连接失败
               ├── 重连 (指数退避)
               └── ErrorLogger::log_error(MONGO_CONNECT)
```

---

## 8. 配置参数体系

### 8.1 编译期常量 (config.hpp)

```cpp
// PostGIS
DEFAULT_POSTGIS_BATCH_SIZE = 1000
MAX_POSTGIS_BATCH_SIZE = 10000
MIN_POSTGIS_BATCH_SIZE = 100
MAX_POSTGIS_MEMORY_USAGE_MB = 512
MAX_POSTGIS_RETRIES = 3
POSTGIS_CONNECTION_TIMEOUT_SEC = 30

// MongoDB
DEFAULT_MONGO_BATCH_SIZE = 100
MAX_MONGO_BATCH_SIZE = 1000
MIN_MONGO_BATCH_SIZE = 10
MAX_MONGO_CONNECTION_POOL_SIZE = 50
DEFAULT_MONGO_CONNECTION_POOL_SIZE = 10
MONGO_TIMEOUT_MS = 30000
MONGO_MAX_RETRIES = 3
```

### 8.2 运行时配置 (mongo_config)

```cpp
struct mongo_config {
    // 连接参数
    host, port, dbname, collection, username, password, auth_source
    
    // 性能参数
    batch_size = 100
    connection_pool_size = 10
    timeout_ms = 30000
    max_retries = 3
    
    // 写入参数
    write_concern_level = PRIMARY  // NONE | PRIMARY | MAJORITY
    journal = false
    wtimeout_ms = 5000
    
    // 集合管理
    create_indexes = true
    drop_collection_before_write = false
    
    // 元数据
    write_metadata = false

    // 监控
    enable_progress_report = true
};
```

---

## 9. 构建系统

### 9.1 依赖库

| 库 | 版本要求 | 用途 |
|----|---------|------|
| libpq | PostgreSQL 9.0+ | PostGIS 连接 |
| mongocxx | 3.6+ | MongoDB C++ 驱动 |
| bsoncxx | 3.6+ | BSON 序列化 |
| sqlite3 | 3.x | MBTiles + 错误日志 |
| zlib | 1.x | 瓦片压缩 |
| libpthread | POSIX | 多线程 |

### 9.2 编译命令

```bash
# Debug 构建
make tippecanoe-db

# Release 构建
BUILDTYPE=Release make tippecanoe-db
```

### 9.3 目标文件清单

```
tippecanoe-db 依赖的 .o 文件:
  核心模块: maindb.o, tile-db.o, geojson.o, serial.o, geometry.o, ...
  PostGIS:  postgis.o, postgis_manager.o, wkb_parser.o
  MongoDB:  mongo.o, mongo_manager.o
  错误日志: error_logger.o
  第三方:   clipper2/src/clipper.engine.o, jsonpull/jsonpull.o
```
