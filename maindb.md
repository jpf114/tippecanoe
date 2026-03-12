# tippecanoe-db 工具说明

## 功能设计

tippecanoe-db 是一个专门用于从 PostGIS 数据库读取地理空间数据并生成 MBTiles 或目录格式地图瓦片的工具。它是 tippecanoe 项目的一个变体，专注于直接从空间数据库获取数据，而不是从文件读取。

### 核心功能

1. **PostGIS 数据库连接**：直接连接到 PostGIS 数据库，支持通过表名或 SQL 查询获取空间数据
2. **空间数据处理**：对从数据库读取的几何数据进行处理，包括：
   - 几何简化
   - 特征聚类
   - 属性过滤
   - 空间索引
3. **瓦片生成**：生成多缩放级别的地图瓦片，支持 MBTiles 和目录格式输出
4. **智能缩放级别选择**：可以根据数据密度自动选择合适的最大缩放级别
5. **高度可配置**：提供丰富的命令行选项，用于控制瓦片生成过程的各个方面

### 技术特点

- **多线程处理**：利用多核 CPU 并行处理数据，提高生成效率
- **内存管理**：智能管理内存使用，处理大规模数据集
- **磁盘空间管理**：监控磁盘空间使用，避免因空间不足导致的失败
- **错误处理**：提供详细的错误信息，便于调试

## 与普通 tippecanoe 的区别

| 特性 | tippecanoe | tippecanoe-db |
|------|------------|---------------|
| 输入源 | 文件（GeoJSON、GeoCSV 等） | PostGIS 数据库 |
| 连接配置 | 不需要数据库连接参数 | 需要提供 PostGIS 连接参数 |
| 数据读取方式 | 从文件系统读取 | 通过数据库查询读取 |
| 支持的数据源格式 | 多种文件格式 | 仅支持 PostGIS 数据库 |
| 命令行选项 | 通用选项 | 包含 PostGIS 特定选项 |
| 核心处理逻辑 | 相同 | 相同（除数据读取部分） |

## 使用说明

### 基本语法

```bash
tippecanoe-db [选项] -o 输出文件.mbtiles
```

### 必需参数

| 参数 | 说明 | 示例 |
|------|------|------|
| `--postgis` 或相关选项 | PostGIS 数据库连接参数 | `--postgis=host:port:dbname:user:password:table:geometry_field` |
| `-o` 或 `--output` | 输出 MBTiles 文件路径 | `-o output.mbtiles` |

### 常用选项

#### PostGIS 连接选项

- `--postgis=host:port:dbname:user:password:table:geometry_field[:sql]`：完整的 PostGIS 连接参数
- `--postgis-host=HOST`：数据库主机
- `--postgis-port=PORT`：数据库端口（默认 5432）
- `--postgis-dbname=DBNAME`：数据库名称
- `--postgis-user=USER`：用户名
- `--postgis-password=PASSWORD`：密码
- `--postgis-table=TABLE`：表名
- `--postgis-geometry-field=FIELD`：几何字段名
- `--postgis-sql=SQL`：自定义 SQL 查询

#### 瓦片生成选项

- `-z, --maximum-zoom=ZOOM`：最大缩放级别（默认 14）
- `-Z, --minimum-zoom=ZOOM`：最小缩放级别（默认 0）
- `-B, --base-zoom=ZOOM`：基础缩放级别
- `-r, --drop-rate=RATE`：特征丢弃率（默认 2.5）
- `-s, --simplification=AMOUNT`：几何简化程度（默认 1）
- `-k, --cluster-maxzoom=ZOOM`：聚类最大缩放级别
- `-K, --cluster-distance=DISTANCE`：聚类距离

#### 输出选项

- `-o, --output=FILE`：输出 MBTiles 文件
- `-e, --output-to-directory=DIR`：输出到目录而不是 MBTiles
- `-f, --force`：覆盖现有文件

### 使用示例

#### 基本用法

从 PostGIS 数据库读取数据并生成 MBTiles 文件：

```bash
tippecanoe-db --postgis=localhost:5432:gis:user:password:points:geom -o points.mbtiles
```

#### 使用自定义 SQL 查询

```bash
tippecanoe-db --postgis-host=localhost --postgis-port=5432 --postgis-dbname=gis \
  --postgis-user=user --postgis-password=password \
  --postgis-sql="SELECT id, name, geom FROM points WHERE population > 1000" \
  -o cities.mbtiles
```

#### 控制缩放级别和简化

```bash
tippecanoe-db --postgis=localhost:5432:gis:user:password:roads:geom \
  -z 12 -Z 5 -s 2 -B 8 \
  -o roads.mbtiles
```

## 高级功能

### 特征过滤

- `-x, --exclude=ATTRIBUTES`：排除指定属性
- `-y, --include=ATTRIBUTES`：仅包含指定属性
- `-j, --feature-filter=FILTER`：基于属性的特征过滤器

### 性能优化

- `--temporary-directory=DIR`：指定临时目录
- `--no-tile-size-limit`：禁用瓦片大小限制
- `--no-feature-limit`：禁用特征数量限制

### 高级几何处理

- `--detect-shared-borders`：检测共享边界
- `--no-line-simplification`：禁用线条简化
- `--no-tiny-polygon-reduction`：禁用小多边形减少

## 常见问题

### 连接失败

确保 PostGIS 数据库服务正在运行，并且连接参数正确。检查网络连接和数据库权限。

### 内存不足

对于大型数据集，可以使用 `--temporary-directory` 指定具有更多空间的临时目录，并考虑降低 `-z` 值以减少生成的瓦片数量。

### 瓦片大小超限

使用 `-s` 增加简化程度，或使用 `--drop-densest-as-needed` 自动减少密集区域的特征。

## 总结

tippecanoe-db 是一个强大的工具，专门用于从 PostGIS 数据库直接生成地图瓦片。它继承了 tippecanoe 的核心功能，同时添加了对数据库连接的支持，为空间数据可视化提供了便捷的解决方案。通过合理配置参数，可以生成高质量、性能优化的地图瓦片，满足各种空间数据可视化需求。