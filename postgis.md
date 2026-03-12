# Tippecanoe 的 PostGIS 支持

## 概述

本文档描述了为 Tippecanoe 添加的 PostGIS 支持，使其能够直接从 PostgreSQL/PostGIS 数据库读取地理空间数据。

## 已实现的功能

1. **直接数据库连接** - 使用 PQconnectdbParams 安全连接到 PostgreSQL/PostGIS 数据库
2. **空间数据查询** - 执行 SQL 查询以检索地理空间数据，支持自定义 SQL 或自动生成
3. **几何数据转换** - 将 PostGIS 几何数据转换为 GeoJSON 格式，再由 Tippecanoe 内部处理
4. **无缝集成** - 与现有的 Tippecanoe GeoJSON 解析流程集成
5. **灵活配置** - 支持单行和单独参数配置
6. **数据类型处理** - 自动处理不同 PostgreSQL 数据类型（数值、布尔、字符串）

## 技术实现

### 修改/添加的文件

1. **postgis.hpp** - 定义 PostGIS 配置结构和 PostGISReader 类的头文件
2. **postgis.cpp** - 实现 PostGIS 数据库连接和数据读取
3. **main.cpp** - 添加 PostGIS 命令行选项和数据读取逻辑
4. **Makefile** - 添加 PostgreSQL/PostGIS 依赖

### 关键组件

#### PostGIS 配置

```cpp
struct postgis_config {
    std::string host;           // 数据库主机
    std::string port;           // 数据库端口
    std::string dbname;         // 数据库名称
    std::string user;           // 数据库用户
    std::string password;       // 数据库密码
    std::string table;          // 数据表名称
    std::string sql;            // SQL查询语句
    std::string geometry_field; // 几何字段名称
};
```

#### PostGISReader 类

`PostGISReader` 类处理：
- 使用 libpq 的 PQconnectdbParams 进行安全的数据库连接
- SQL 查询执行，支持自定义 SQL 或基于表和几何字段自动生成
- 几何数据转换：使用 ST_AsGeoJSON 将 PostGIS 几何转换为 GeoJSON 格式
- 自动几何列检测：优先使用 'geojson' 列，其次使用指定的几何字段，最后使用第一列作为 fallback
- 数据类型处理：自动处理数值、布尔和字符串类型
- 要素序列化：构建 GeoJSON FeatureCollection 并使用 Tippecanoe 现有的 GeoJSON 解析逻辑

## 安装

### 前提条件

- PostgreSQL 客户端库 (libpq)
- 在 PostgreSQL 数据库中安装了 PostGIS 扩展
- 兼容 C++17 的编译器

### 构建说明

1. **安装 PostgreSQL 客户端库**：
   - Ubuntu/Debian: `sudo apt-get install libpq-dev`
   - CentOS/RHEL: `sudo yum install postgresql-devel`
   - macOS: `brew install postgresql`

2. **构建 Tippecanoe**：
   ```bash
   make -j
   make install
   ```

## 使用方法

### 命令行选项

#### 单行配置

```bash
# 基本连接
 tippecanoe -o output.mbtiles --postgis "host:port:dbname:user:password" --postgis-sql "SELECT id, name, ST_AsGeoJSON(geom) as geojson FROM table WHERE condition"

# 带表名和几何字段
 tippecanoe -o output.mbtiles --postgis "host:port:dbname:user:password:table:geometry_column"

# 带表名、几何字段和条件
 tippecanoe -o output.mbtiles --postgis "host:port:dbname:user:password:table:geometry_column:condition"
```

#### 单独参数

```bash
tippecanoe -o output.mbtiles \
  --postgis-host localhost \
  --postgis-port 5432 \
  --postgis-dbname gis \
  --postgis-user postgres \
  --postgis-password password \
  --postgis-sql "SELECT id, name, ST_AsGeoJSON(geom) as geojson FROM roads WHERE highway='motorway'"

# 使用表名和几何字段（自动生成SQL）
tippecanoe -o output.mbtiles \
  --postgis-host localhost \
  --postgis-port 5432 \
  --postgis-dbname gis \
  --postgis-user postgres \
  --postgis-password password \
  --postgis-table roads \
  --postgis-geometry-field geom
```

### 选项详情

| 选项 | 描述 | 默认值 |
|--------|-------------|---------|
| `--postgis` | 组合的 PostGIS 连接字符串，格式：host:port:dbname:user:password:table:geometry_column:condition | N/A |
| `--postgis-host` | 数据库主机 | localhost |
| `--postgis-port` | 数据库端口 | 5432 |
| `--postgis-dbname` | 数据库名称 | (必需) |
| `--postgis-user` | 数据库用户 | (必需) |
| `--postgis-password` | 数据库密码 | (必需) |
| `--postgis-table` | 数据表名称 | (可选，与geometry-field一起使用时自动生成SQL) |
| `--postgis-geometry-field` | 几何字段名称 | (可选，默认为"geometry"，与table一起使用时自动生成SQL) |
| `--postgis-sql` | 自定义 SQL 查询语句 | (可选，当提供表名和几何字段时自动生成) |

## 使用 --postgis-sql 选项直接输入 SQL 查询语句

### 功能说明

`--postgis-sql` 选项允许用户直接输入完整的 SQL 查询语句来从 PostGIS 数据库中获取地理空间数据。当使用此选项时，系统会直接执行用户提供的 SQL 查询。

### 基本语法

```bash
tippecanoe --postgis-host=<主机名> --postgis-port=<端口> --postgis-dbname=<数据库名> --postgis-user=<用户名> --postgis-password=<密码> --postgis-sql="<SQL 查询语句>" -o <输出文件>.mbtiles
```

### 示例

#### 1. 基本查询

```bash
tippecanoe --postgis-host=localhost --postgis-port=5432 --postgis-dbname=gis --postgis-user=postgres --postgis-password=password --postgis-sql="SELECT id, name, ST_AsGeoJSON(geom) as geojson FROM roads WHERE type='highway'" -o roads.mbtiles
```

#### 2. 复杂查询

```bash
tippecanoe --postgis-host=localhost --postgis-port=5432 --postgis-dbname=gis --postgis-user=postgres --postgis-password=password --postgis-sql="SELECT b.id, b.name, ST_AsGeoJSON(b.geom) as geojson FROM buildings b JOIN zones z ON ST_Intersects(b.geom, z.geom) WHERE z.type='residential'" -o residential_buildings.mbtiles
```

#### 3. 使用空间函数

```bash
tippecanoe --postgis-host=localhost --postgis-port=5432 --postgis-dbname=gis --postgis-user=postgres --postgis-password=password --postgis-sql="SELECT id, name, ST_AsGeoJSON(ST_Simplify(geom, 0.001)) as geojson FROM countries" -o countries_simplified.mbtiles
```

### 注意事项

1. **查询结果必须包含 GeoJSON 格式的几何列**：系统会尝试以下方式查找几何列：
   - 首先查找名为 'geojson' 的列（推荐使用）
   - 然后尝试使用 `--postgis-geometry-field` 指定的列名
   - 如果仍然找不到，使用第一列作为 fallback
   因此，您的查询语句中应该包含 `ST_AsGeoJSON(geometry_column) as geojson` 或类似的表达式。

2. **连接参数仍然需要提供**：即使使用自定义 SQL 查询，您仍然需要提供数据库连接参数（主机、端口、数据库名、用户名、密码）。

3. **自动 SQL 生成**：当您在 `--postgis` 选项中提供表名和几何字段，或使用 `--postgis-table` 和 `--postgis-geometry-field` 选项时，系统会自动生成 SQL 查询语句：
   - 基本格式：`SELECT ST_AsGeoJSON(geometry_column) as geojson, * FROM table`

4. **查询性能**：对于大型数据库，建议在查询中添加适当的过滤条件，以避免返回过多数据。

5. **SQL 语法**：请确保您的 SQL 语法正确，并且在命令行中正确转义特殊字符。

### 错误处理

如果 SQL 查询执行失败，系统会显示错误消息并退出。请检查您的 SQL 语法和数据库连接参数。

## 限制

1. **几何类型** - 当前支持点、线串和多边形几何类型
2. **性能** - 对于大型数据集，考虑使用适当的 WHERE 子句限制数据大小
3. **错误处理** - 数据库连接和查询执行的基本错误处理
4. **依赖** - 需要安装 PostgreSQL 客户端库
5. **数据量** - 当前实现中，查询结果会被完全加载到内存中，对于大型数据集可能会有内存限制
6. **测试模式** - 当前实现默认只处理前3个数据记录（硬编码限制），用于测试目的。需要修改代码中的循环条件以处理完整数据集。

## 故障排除

### 连接问题

- 确保 PostgreSQL 服务器正在运行
- 验证主机、端口、用户名和密码
- 检查防火墙设置

### 查询问题

- 确保几何列存在并正确索引
- 验证 WHERE 子句是有效的 SQL
- 检查用户是否有足够的权限
- 确保查询返回 GeoJSON 格式的几何数据

### 构建问题

- 确保安装了 libpq-dev
- 检查编译器兼容性 (需要 C++17)
- 验证 Makefile 具有正确的 PostgreSQL 包含路径

## 未来增强

1. 支持更多几何类型（多点、多线串、多多边形、几何集合）
2. 利用空间索引提高性能
3. 支持查询中的 PostGIS 函数
4. 大型数据集的批处理，减少内存使用
5. 连接池以提高性能
6. 更详细的错误处理和日志记录
7. 移除测试模式限制，支持完整数据集处理