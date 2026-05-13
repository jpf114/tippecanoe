# Tippecanoe 大数据量测试工具包

## 目录说明

```
├── generate_big_data.py     # 数据生成脚本（纯Python，零依赖，兼容Python 2.7+）
├── china_country.geojson    # 源数据：中国行政区划面要素（~10MB）
├── run_generate.sh          # 一键生成数据脚本
├── run_tile.sh              # 一键切片执行脚本
└── README.txt               # 本文件
```

## 系统要求

- Linux x86_64 系统
- Python 2.7 或 3.x（系统自带即可，无需任何第三方库）
- 至少 250GB 磁盘空间（生成 200GB 测试数据）

## 第一步：编译 tippecanoe（可选）

当前 `tippecanoe` 可执行文件为**静态链接版本**，不依赖 GLIBC、libstdc++ 等系统库，可在任意 Linux x86_64 系统运行。

如果遇到兼容问题，可在目标机器上自行编译：

```bash
# 安装编译依赖
sudo yum install -y gcc-c++ make zlib-devel sqlite-devel
# 或 apt: sudo apt install -y g++ make zlib1g-dev libsqlite3-dev

# 编译
git clone https://github.com/felt/tippecanoe.git
cd tippecanoe
LDFLAGS="-static -static-libgcc -static-libstdc++" make tippecanoe -j$(nproc)

# 复制到本目录
cp tippecanoe /path/to/tippecanoe_test/
```

## 使用方法

### 1. 赋权

```bash
chmod +x tippecanoe run_generate.sh run_tile.sh
```

### 2. 快速验证

```bash
# 用 china_country.geojson 测试，确认 tippecanoe 能正常运行
./tippecanoe -o test.mbtiles -f -l test china_country.geojson
```

### 3. 生成测试数据（约 200GB，~10亿要素）

```bash
./run_generate.sh

# 或者手动指定参数：
python generate_big_data.py \
    -i china_country.geojson \
    -o big_data_200g.geojson \
    --target-gb 200 \
    --subdivide-points 10 \
    --max-coords 6 \
    --progress-interval 500000
```

**参数说明：**
- `--target-gb`: 目标数据大小（GB），默认 200
- `--subdivide-points`: 源面分割密度，越大种子面越多（默认 10）
- `--max-coords`: 每个面最大坐标点数，越小要素越小（默认 6）
- `--progress-interval`: 进度输出间隔（每 N 个要素输出一次）

**数据量参考：**
| 配置 | 每要素大小 | 200GB 要素数 | 生成速度 |
|------|-----------|-------------|---------|
| 默认 (max-coords=6) | ~190 bytes | ~10亿 | ~40-50 MB/s |
| `--max-coords 8` | ~250 bytes | ~8亿 | ~40 MB/s |
| `--max-coords 15` | ~400 bytes | ~5亿 | ~35 MB/s |

### 4. 执行切片

```bash
# 使用默认输入输出路径
./run_tile.sh

# 或者指定输入输出文件
./run_tile.sh big_data_200g.geojson output.mbtiles

# 或直接使用 tippecanoe
./tippecanoe -o output.mbtiles -f -l big_data -z14 -Z0 \
    --drop-densest-as-needed \
    --extend-zooms-if-still-dropping \
    --detect-shared-borders \
    --grid-low-zooms \
    --simplification=10 \
    --buffer=64 \
    --maximum-tile-bytes=500000 \
    --maximum-tile-features=200000 \
    --read-parallel \
    --progress-interval=30 \
    big_data_200g.geojson
```

## 数据生成算法

1. 读取 china_country.geojson 中的面要素坐标环
2. 在每个面范围内做网格划分，在每个网格单元内生成不规则多边形（3-6边，随机抖动）
3. 对种子面做随机偏移 + 缩放 + 旋转，在全球范围内大量复制
4. 流式输出 NDGeoJSON（每行一个 Feature，无需 FeatureCollection 包裹）

## 注意事项

- 本脚本为零依赖纯 Python，系统自带的 python2/python3 均可运行
- 生成的 NDGeoJSON 文件极大（200GB+），请确保目标磁盘有足够空间
- 切片过程可能需要较长时间（取决于 CPU 和磁盘 IO）
- 切片过程中产生的临时文件可能需要额外几十 GB 空间
- 确保目标机器有足够的内存（建议 8GB 以上）
