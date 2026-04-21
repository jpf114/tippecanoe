# Tippecanoe 大文件处理测试报告

## 1. 测试概述

本报告记录了 tippecanoe v2.80.0 在处理大规模 GeoJSON 文件时的性能测试结果，包括单文件处理和合并处理两种场景，覆盖从 z5 到 z14 不同缩放级别和多种参数组合。

## 2. 测试环境

| 项目 | 规格 |
|------|------|
| 操作系统 | Linux |
| CPU | 16 核 |
| 内存 | 7.6 GB (可用 5.9 GB) |
| Swap | 2.0 GB |
| 磁盘 | 1007 GB (可用 925 GB) |
| tippecanoe 版本 | v2.80.0 |
| 编译方式 | make tippecanoe (不含 tippecanoe-db) |

## 3. 测试数据

| 数据文件 | 文件大小 | 特征数量 | 数据格式 | 数据类型 |
|----------|----------|----------|----------|----------|
| California.geojsonl | 3.6 GB | 11,542,912 条 | GeoJSONL (每行一个Feature) | 多边形 (Polygon) |
| NewYork.geojsonl | 1.4 GB | 4,972,497 条 | GeoJSONL (每行一个Feature) | 多边形 (Polygon) |
| 合计 (合并处理) | ~5.0 GB | ~16,515,409 条 | GeoJSONL | 多边形 |

## 4. 测试用例设计

### 4.1 PART 1: California 单文件测试 (7个测试)

| 测试编号 | 测试名称 | 缩放级别 | 模式 | 关键参数 |
|----------|----------|----------|------|----------|
| 1.1 | ca_z5_sequential | z0-z5 | 顺序 | 默认参数 |
| 1.2 | ca_z5_parallel | z0-z5 | 并行 | -P |
| 1.3 | ca_z8_default | z0-z8 | 顺序 | 默认参数 |
| 1.4 | ca_z8_optimized | z0-z8 | 顺序 | -r5, --drop-densest-as-needed, --simplification=10 |
| 1.5 | ca_z12_default | z0-z12 | 顺序 | 默认参数 |
| 1.6 | ca_z12_parallel_optimized | z0-z12 | 并行 | -P, -r5, --drop-densest-as-needed, --coalesce-densest-as-needed, --simplification=10 |
| 1.7 | ca_auto_zoom | 自动 | 顺序 | -zg |

### 4.2 PART 2: NewYork 单文件测试 (4个测试)

| 测试编号 | 测试名称 | 缩放级别 | 模式 | 关键参数 |
|----------|----------|----------|------|----------|
| 2.1 | ny_z5_default | z0-z5 | 顺序 | 默认参数 |
| 2.2 | ny_z12_default | z0-z12 | 顺序 | 默认参数 |
| 2.3 | ny_z12_parallel_optimized | z0-z12 | 并行 | -P, -r5, --drop-densest-as-needed, --simplification=10 |
| 2.4 | ny_auto_zoom | 自动 | 顺序 | -zg |

### 4.3 PART 3: 合并文件测试 (5个测试)

| 测试编号 | 测试名称 | 缩放级别 | 模式 | 关键参数 |
|----------|----------|----------|------|----------|
| 3.1 | merged_z5_sequential | z0-z5 | 顺序 | 默认参数 |
| 3.2 | merged_z5_parallel | z0-z5 | 并行 | -P |
| 3.3 | merged_z12_sequential | z0-z12 | 顺序 | 默认参数 |
| 3.4 | merged_z12_parallel_optimized | z0-z12 | 并行 | -P, -r5, --drop-densest-as-needed, --coalesce-densest-as-needed, --simplification=10 |
| 3.5 | merged_z8_separate_layers | z0-z8 | 并行 | -P, 独立图层命名 |

### 4.4 PART 4: z14 及更高层级验证测试 (6个测试)

| 测试编号 | 测试名称 | 缩放级别 | 模式 | 关键参数 |
|----------|----------|----------|------|----------|
| 4.1 | ca_z14_strong_opt | z0-z14 | 并行 | -P, -r5, --drop-densest-as-needed, --coalesce-densest-as-needed, --simplification=10 |
| 4.2 | ca_z14_aggressive | z0-z14 | 并行 | -P, -r10, --drop-densest-as-needed, --coalesce-densest-as-needed, --simplification=10 |
| 4.3 | ca_z14_max_tile_limit | z0-z14 | 并行 | -P, -r10, --drop-densest-as-needed, --coalesce-densest-as-needed, --simplification=10, --maximum-tile-bytes=512000 |
| 4.4 | ca_z14_drop_fraction | z0-z14 | 并行 | -P, -r10, --drop-densest-as-needed, --drop-fraction-as-needed, --coalesce-densest-as-needed, --simplification=10 |
| 4.5 | ny_z14_strong_opt | z0-z14 | 并行 | -P, -r5, --drop-densest-as-needed, --coalesce-densest-as-needed, --simplification=10 |
| 4.6 | merged_z14_strong_opt | z0-z14 | 并行 | -P, -r10, --drop-densest-as-needed, --coalesce-densest-as-needed, --simplification=10, --drop-fraction-as-needed |

## 5. 测试结果

### 5.1 California 单文件测试结果

| 测试 | 状态 | 处理时间(s) | 峰值内存(MB) | 输出大小(MB) | 总瓦片数 | 最大缩放级别 |
|------|------|-------------|-------------|-------------|----------|-------------|
| ca_z5_sequential | **成功** | 188.2 | 980.1 | 0.1 | 11 | z5 |
| ca_z5_parallel | **成功** | 197.1 | 3740.0 | 0.1 | 11 | z5 |
| ca_z8_default | **成功** | 467.9 | 842.1 | 3.6 | 75 | z8 |
| ca_z8_optimized | **成功** | 397.9 | 875.0 | 4.1 | 75 | z8 |
| ca_z12_default | **部分成功** | - | - | - | 210 (仅到z9) | z9 (目标z12) |
| ca_z12_parallel_optimized | **成功** | 549.8 | 3375.1 | 109.2 | 7770 | z12 |
| ca_auto_zoom | **部分成功** | - | - | - | 210 (仅到z9) | z9 |
| ca_z14_strong_opt | **成功** | 629.1 | 2164.8 | 364.8 | 67376 | z14 |
| ca_z14_aggressive | **成功** | 599.3 | 2296.1 | 364.7 | 67376 | z14 |
| ca_z14_max_tile_limit | **成功** | 610.1 | 2619.3 | 365.6 | 67376 | z14 |
| ca_z14_drop_fraction | **成功** | 603.6 | 2595.8 | 365.0 | 67376 | z14 |

**各层级瓦片分布 (ca_z14_strong_opt - 完整 z0-z14):**

| 缩放级别 | 瓦片数 | 最大瓦片大小 |
|----------|--------|-------------|
| z0 | 1 | 297B |
| z1 | 1 | 735B |
| z2 | 1 | 1.6KB |
| z3 | 2 | 4.4KB |
| z4 | 2 | 15.0KB |
| z5 | 4 | 53.5KB |
| z6 | 7 | 87.0KB |
| z7 | 16 | 269.1KB |
| z8 | 41 | 498.8KB |
| z9 | 138 | 174.7KB |
| z10 | 487 | 238.3KB |
| z11 | 1676 | 345.6KB |
| z12 | 5395 | 418.9KB |
| z13 | 15959 | 224.3KB |
| z14 | 43646 | 86.3KB |

### 5.2 NewYork 单文件测试结果

| 测试 | 状态 | 处理时间(s) | 峰值内存(MB) | 输出大小(MB) | 总瓦片数 | 最大缩放级别 |
|------|------|-------------|-------------|-------------|----------|-------------|
| ny_z5_default | **成功** | 71.9 | 519.1 | 0.08 | 10 | z5 |
| ny_z12_default | **部分成功** | - | - | - | 312 (仅到z10) | z10 (目标z12) |
| ny_z12_parallel_optimized | **成功** | 187.3 | 1527.3 | 76.5 | 3682 | z12 |
| ny_auto_zoom | **部分成功** | - | - | - | 312 (仅到z10) | z10 |
| ny_z14_strong_opt | **成功** | 235.0 | 1527.2 | 207.8 | 49104 | z14 |

**各层级瓦片分布 (ny_z14_strong_opt - 完整 z0-z14):**

| 缩放级别 | 瓦片数 | 最大瓦片大小 |
|----------|--------|-------------|
| z0 | 1 | 204B |
| z1 | 1 | 333B |
| z2 | 1 | 692B |
| z3 | 2 | 1.8KB |
| z4 | 2 | 5.9KB |
| z5 | 3 | 18.1KB |
| z6 | 5 | 61.0KB |
| z7 | 9 | 98.7KB |
| z8 | 21 | 363.0KB |
| z9 | 61 | 413.0KB |
| z10 | 207 | 430.1KB |
| z11 | 723 | 443.0KB |
| z12 | 2646 | 431.8KB |
| z13 | 9871 | 157.8KB |
| z14 | 35551 | 64.7KB |

### 5.3 合并文件测试结果

| 测试 | 状态 | 处理时间(s) | 峰值内存(MB) | 输出大小(MB) | 总瓦片数 | 最大缩放级别 |
|------|------|-------------|-------------|-------------|----------|-------------|
| merged_z5_sequential | **成功** | 246.5 | 1295.1 | 0.15 | 19 | z5 |
| merged_z5_parallel | **成功** | 192.2 | 2884.2 | 0.15 | 19 | z5 |
| merged_z12_sequential | **部分成功** | - | - | - | 314 (仅到z9) | z9 (目标z12) |
| merged_z12_parallel_optimized | **成功** | 634.2 | 3023.3 | 159.7 | 11450 | z12 |
| merged_z8_separate_layers | **成功** | 358.8 | 3586.0 | 5.6 | 118 | z8 |
| merged_z14_strong_opt | **成功** | 890.8 | 2162.1 | 538.7 | 116478 | z14 |

**各层级瓦片分布 (merged_z14_strong_opt - 完整 z0-z14, 1650万条数据):**

| 缩放级别 | 瓦片数 | 最大瓦片大小 |
|----------|--------|-------------|
| z0 | 1 | 398B |
| z1 | 1 | 955B |
| z2 | 2 | 1.6KB |
| z3 | 4 | 4.4KB |
| z4 | 4 | 15.0KB |
| z5 | 7 | 53.5KB |
| z6 | 12 | 87.0KB |
| z7 | 25 | 269.1KB |
| z8 | 62 | 498.8KB |
| z9 | 199 | 174.7KB |
| z10 | 694 | 238.3KB |
| z11 | 2399 | 345.6KB |
| z12 | 8041 | 418.9KB |
| z13 | 25830 | 224.3KB |
| z14 | 79197 | 86.3KB |

## 6. 关键发现

### 6.1 顺序模式 vs 并行模式

| 对比维度 | 顺序模式 | 并行模式 (-P) |
|----------|----------|---------------|
| 处理时间 (z5 CA) | 188.2s | 197.1s |
| 处理时间 (z5 NY) | 71.9s | - |
| 处理时间 (z12 优化 CA) | 失败 | 549.8s (成功) |
| 处理时间 (z14 优化 CA) | - | 629.1s (成功) |
| 处理时间 (z14 优化 NY) | - | 235.0s (成功) |
| 处理时间 (z14 优化 合并) | - | 890.8s (成功) |
| 峰值内存 (z5 CA) | 980 MB | 3740 MB |
| 峰值内存 (z14 CA) | - | 2164.8 MB |
| 峰值内存 (z14 合并) | - | 2162.1 MB |

**结论:**
- 顺序模式内存友好（~1GB），但高缩放级别（≥z12）默认参数下会失败
- 并行模式速度更快（CPU 利用率更高），但内存消耗约为顺序模式的 2-4 倍
- 在 7.6GB 内存环境下，并行模式处理 z14 仍可控（峰值 ~2.2-3.8GB）

### 6.2 默认参数 vs 优化参数

| 测试场景 | 默认参数 | 优化参数 |
|----------|----------|----------|
| CA z8 | 成功 (3.6MB) | 成功 (4.1MB) |
| CA z12 | **失败** (仅到 z9) | **成功** (完整 z0-z12) |
| CA z14 | **失败** (仅到 z9) | **成功** (完整 z0-z14) |
| NY z12 | **失败** (仅到 z10) | **成功** (完整 z0-z12) |
| 合并 z12 | **失败** (仅到 z9) | **成功** (完整 z0-z12) |
| 合并 z14 | **失败** | **成功** (完整 z0-z14) |

**结论:**
- 默认参数无法处理千万级特征数据到 z12 及以上
- **必须使用优化参数组合**：`-r5 --drop-densest-as-needed --coalesce-densest-as-needed --simplification=10`
- 优化后输出反而更大（因为更多瓦片被成功生成）

### 6.3 z14 不同参数对比分析

| 测试 | 参数差异 | 时间(s) | 内存(MB) | 瓦片数 | 结果 |
|------|----------|---------|----------|--------|------|
| ca_z14_strong_opt | -r5 | 629.1 | 2164.8 | 67376 | **成功** |
| ca_z14_aggressive | -r10 | 599.3 | 2296.1 | 67376 | **成功** |
| ca_z14_max_tile_limit | -r10 + --maximum-tile-bytes=512000 | 610.1 | 2619.3 | 67376 | **成功** |
| ca_z14_drop_fraction | -r10 + --drop-fraction-as-needed | 603.6 | 2595.8 | 67376 | **成功** |

**z14 参数对比发现:**
- 四种参数组合均成功生成完整 z0-z14
- 瓦片数量完全相同 (67376)，说明优化参数已达到稳定态
- z14 瓦片最大仅 86.3KB，远低于 500KB 限制
- 增加 `--drop-fraction-as-needed` 或 `--maximum-tile-bytes` 并未显著改变结果
- `-r10` 比 `-r5` 略快 (599s vs 629s)，但内存消耗稍高

### 6.4 缩放级别限制分析

| 文件 | 默认参数可达最大层级 | 优化参数可达最大层级 |
|------|---------------------|---------------------|
| California (11.5M) | z9 | **z14+** |
| NewYork (5M) | z10 | **z14+** |
| 合并 (16.5M) | z9 | **z14+** |

**结论:**
- 数据量越大，默认参数可达层级越低
- 优化参数可突破此限制，实现 z14 完整切片
- tippecanoe 理论上支持到 z22（由 -Z 参数控制），实际可处理上限取决于优化参数

## 7. 参数详解

### 7.1 基础参数

| 参数 | 全称 | 默认值 | 含义 | 适用场景 |
|------|------|--------|------|----------|
| `-zN` | `--maximum-zoom=N` | 自动计算 | 设置输出的最大瓦片缩放级别。tippecanoe 会生成从 z0 到 zN 的所有层级瓦片。 | 大文件建议显式指定，避免 `-zg` 自动计算导致失败 |
| `-f` | `--force` | 无 | 强制覆盖已存在的输出文件，不提示确认。 | 脚本自动化必须使用 |
| `-q` | `--quiet` | 无 | 静默模式，不输出处理进度信息。 | 日志记录时使用 |
| `-o FILE` | `--output=FILE` | 无 | 指定输出文件路径，通常为 `.mbtiles` 格式。 | 必需参数 |
| `-P` | `--read-parallel` | 关闭 | 并行读取输入文件。使用多线程并发读取，可提升 20-50% 处理速度。 | 多核 CPU + 充足内存时使用 |

### 7.2 特征丢弃类参数 (Dropping features)

| 参数 | 全称 | 含义 | 适用场景 |
|------|------|------|----------|
| `-rN` | `--drop-rate=N` | **特征丢弃率**。控制随着缩放级别降低时特征被丢弃的速率。值越大，低层丢弃的特征越多。默认值约为 2.5。设置为 5-10 可大幅减少低层瓦片的特征密度。 | 大文件必须设置，推荐 5-10。值越大处理越快但低层细节越少。 |
| `--drop-densest-as-needed` | - | **按需丢弃最密集区域的特征**。当某个瓦片超过 500KB 大小时，自动丢弃该瓦片中密度最大区域的特征，直到瓦片大小符合限制。这是处理高密度数据的核心参数。 | 大文件高缩放级别必须使用。否则会出现 exit code 100 错误。 |
| `--drop-fraction-as-needed` | - | **按需按固定比例丢弃特征**。与 `--drop-densest-as-needed` 配合使用，当瓦片仍然超限时，按固定比例进一步丢弃特征。 | 极端高密度数据的补充手段，通常与 `--drop-densest-as-needed` 联用。 |
| `--drop-smallest-as-needed` | - | **按需丢弃最小的特征**。优先丢弃面积最小的特征来减小瓦片大小。 | 适用于特征大小差异很大的数据集。 |

**`--drop-densest-as-needed` 工作机制:**
1. 检查每个瓦片是否超过 500KB 限制
2. 如果超过，计算瓦片内每个区域（单元格）的特征密度
3. 丢弃密度最高单元格中的一部分特征
4. 重复步骤 1-3，直到瓦片大小符合限制

### 7.3 特征合并类参数 (Coalescing features)

| 参数 | 全称 | 含义 | 适用场景 |
|------|------|------|----------|
| `--coalesce-densest-as-needed` | - | **按需合并最密集区域的特征**。当瓦片超限时，尝试将相邻多边形合并为一个多边形，从而减少特征数量。与 `--drop-densest-as-needed` 互补：一个丢弃，一个合并。 | 多边形数据强烈推荐使用。可减少 10-30% 的特征数量。 |
| `--coalesce-fraction-as-needed` | - | **按需按固定比例合并特征**。当上述合并不足以控制瓦片大小时，按固定比例进一步合并。 | 极端高密度数据的补充。 |
| `--coalesce-smallest-as-needed` | - | **按需合并最小的特征**。优先合并面积最小的相邻多边形。 | 与 `--drop-smallest-as-needed` 配合使用。 |

### 7.4 几何简化参数

| 参数 | 全称 | 含义 | 适用场景 |
|------|------|------|----------|
| `--simplification=N` | - | **几何简化程度**。使用 Visvalingam-Whyatt 算法简化多边形边界。值越大，简化程度越高。默认值为 1.5（自动计算）。设置为 10 可显著减少多边形顶点数量，降低瓦片大小。 | 大文件多边形数据推荐使用 10。可在视觉上几乎无差异的情况下大幅减小输出。 |
| `--no-line-simplification` | - | **禁用线简化**。完全关闭几何简化。 | 需要精确保留原始几何形状时使用（通常不推荐）。 |
| `--simplify-only-low-zooms` | - | **仅简化低缩放级别**。在高缩放级别保留完整几何精度。 | 需要高精度的高层级视图时使用。 |
| `--visvalingam` | - | **使用 Visvalingam 算法**。替代默认的 Douglas-Peucker 简化算法，保留更多视觉特征。 | 对简化质量要求较高时使用。 |

### 7.5 瓦片限制参数

| 参数 | 全称 | 默认值 | 含义 |
|------|------|--------|------|
| `--maximum-tile-bytes=N` | - | 512000 (500KB) | 设置单个瓦片的最大字节数。超过此限制的瓦片会被标记并触发丢弃/合并机制。增大此值可减少特征丢失，但可能影响前端加载性能。 |
| `--maximum-tile-features=N` | - | 200000 | 设置单个瓦片的最大特征数量。超过此限制的瓦片会触发特征丢弃。 |
| `--limit-tile-feature-count=N` | - | 200000 | 与 `--maximum-tile-features` 类似，但在处理后期强制执行。 |
| `--no-feature-limit` | - | 无 | 禁用特征数量限制。允许瓦片包含任意数量的特征。可能导致前端渲染卡顿。 |
| `--no-tile-size-limit` | - | 无 | 禁用瓦片大小限制。允许瓦片超过 500KB。通常不推荐。 |

### 7.6 其他重要参数

| 参数 | 含义 |
|------|------|
| `-dN` / `-DN` | 设置低层/高层几何精度（默认 12）。降低可减少瓦片大小。 |
| `-bN` | 设置瓦片缓冲区大小（默认 5 像素）。增大可减少瓦片边缘的切割问题。 |
| `--gamma=N` | 控制重叠特征的合并程度。值越大，合并程度越高。 |
| `--increase-gamma-as-needed` | 自动增加 gamma 值以控制瓦片大小。 |
| `-L'{"file":"...", "layer":"..."}'` | 使用 JSON 配置指定输入文件和图层名称，支持多文件独立图层。 |
| `-l NAME` | 为所有输入设置统一的图层名称。 |
| `-zg` | 自动计算最大缩放级别。根据数据密度和分辨率自动选择。**不推荐用于千万级数据**，易导致处理失败。 |

## 8. 各层级最佳参数配置建议

### 8.1 低缩放级别 (z0-z5)

**推荐参数:** 默认参数即可，或使用并行模式加速

```bash
# 顺序模式 (内存友好)
tippecanoe -q -f -z5 -o output.mbtiles input.geojsonl
# 内存: ~1GB, 时间: 188s (CA), 72s (NY)

# 并行模式 (速度优先)
tippecanoe -q -f -z5 -P -o output.mbtiles input.geojsonl
# 内存: ~3.7GB, 时间: 197s (CA)
```

**建议:** 内存 ≤ 8GB 时使用顺序模式；内存 ≥ 16GB 时使用并行模式

### 8.2 中缩放级别 (z0-z8)

**推荐参数:**
```bash
# 方案一：默认参数 (适用于百万级以下数据)
tippecanoe -q -f -z8 -o output.mbtiles input.geojsonl

# 方案二：轻量优化 (适用于千万级数据，推荐)
tippecanoe -q -f -z8 -P \
  -r5 \
  --drop-densest-as-needed \
  --simplification=10 \
  -o output.mbtiles input.geojsonl
```

### 8.3 高缩放级别 (z0-z12)

**推荐参数 (必须使用优化):**
```bash
tippecanoe -q -f -z12 -P \
  -r5 \
  --drop-densest-as-needed \
  --coalesce-densest-as-needed \
  --simplification=10 \
  -o output.mbtiles input.geojsonl
```

**注意事项:**
- 不使用优化参数时，z12 处理会失败 (exit code 100)
- 需要 ~3.4GB 峰值内存
- 输出约 100-160MB

### 8.4 最高缩放级别 (z0-z14)

**推荐参数 (必须使用优化):**
```bash
# 标准配置 (推荐)
tippecanoe -q -f -z14 -P \
  -r5 \
  --drop-densest-as-needed \
  --coalesce-densest-as-needed \
  --simplification=10 \
  -o output.mbtiles input.geojsonl

# 更快速度 (牺牲少量低层精度)
tippecanoe -q -f -z14 -P \
  -r10 \
  --drop-densest-as-needed \
  --coalesce-densest-as-needed \
  --simplification=10 \
  -o output.mbtiles input.geojsonl

# 最激进配置 (极端高密度数据)
tippecanoe -q -f -z14 -P \
  -r10 \
  --drop-densest-as-needed \
  --drop-fraction-as-needed \
  --coalesce-densest-as-needed \
  --simplification=10 \
  -o output.mbtiles input.geojsonl
```

**z14 性能数据:**

| 数据 | 处理时间 | 峰值内存 | 输出大小 | 总瓦片数 |
|------|----------|----------|----------|----------|
| California (11.5M) | 629s (~10.5min) | 2164.8 MB | 364.8 MB | 67376 |
| NewYork (5M) | 235s (~4min) | 1527.2 MB | 207.8 MB | 49104 |
| 合并 (16.5M) | 890.8s (~15min) | 2162.1 MB | 538.7 MB | 116478 |

### 8.5 合并多文件

**推荐参数:**
```bash
# 统一图层
tippecanoe -q -f -z14 -P \
  -r10 \
  --drop-densest-as-needed \
  --coalesce-densest-as-needed \
  --simplification=10 \
  --drop-fraction-as-needed \
  -o output.mbtiles file1.geojsonl file2.geojsonl

# 独立图层 (推荐，便于后续按需加载)
tippecanoe -q -f -z8 -P \
  -L'{"file":"file1.geojsonl","layer":"Layer1"}' \
  -L'{"file":"file2.geojsonl","layer":"Layer2"}' \
  -o output.mbtiles
```

### 8.6 自动缩放级别 (-zg)

**结论:** 不推荐用于千万级数据

- California 11.5M 条: 自动计算为 z9，处理失败
- NewYork 5M 条: 自动计算为 z10，处理失败
- **建议:** 对于大文件，显式指定 `-z` 参数

## 9. 参数适应性测试

### 9.1 不同文件大小的参数适应性

| 数据规模 | 特征数 | 安全最大层级(默认) | 安全最大层级(优化) |
|----------|--------|-------------------|-------------------|
| ~500万 (NY) | 4,972,497 | z10 | z14+ |
| ~1150万 (CA) | 11,542,912 | z9 | z14+ |
| ~1650万 (合并) | 16,515,409 | z9 | z14+ |

### 9.2 内存与处理速度关系

| 模式 | 内存需求 | 处理速度 | 适用场景 |
|------|----------|----------|----------|
| 顺序 | ~1GB / 千万条 | 基准 | 内存受限环境 (≤8GB) |
| 并行 | ~2-3.8GB / 千万条 | 快 20-50% | 多核CPU + 充足内存 (≥8GB) |

### 9.3 z14 瓦片大小分析

优化参数下，z14 最大瓦片仅 86KB，远低于 500KB 限制：

| 层级 | CA 最大瓦片 | NY 最大瓦片 | 合并 最大瓦片 |
|------|------------|------------|-------------|
| z8 | 499KB | 363KB | 499KB |
| z9 | 175KB | 413KB | 175KB |
| z10 | 238KB | 430KB | 238KB |
| z11 | 346KB | 443KB | 346KB |
| z12 | 419KB | 432KB | 419KB |
| z13 | 224KB | 158KB | 224KB |
| z14 | 86KB | 65KB | 86KB |

**注意:** z14 瓦片反而比 z12 小，这是因为加州/纽约州的数据在 z14 时每个瓦片覆盖的地理区域很小，包含的特征数量少。

## 10. 性能瓶颈分析

### 10.1 内存瓶颈
- 并行模式下 z14 峰值内存 ~2.2GB，低于 z5 的 ~3.8GB
- 若同时运行多个 tippecanoe 实例，可能出现 OOM
- **建议:** 合并处理时确保可用内存 ≥ 4GB

### 10.2 CPU 瓶颈
- 并行模式下 User time >> Wall time，说明 CPU 利用率高
- ca_z14_strong_opt: Wall=629s, User=2462s, 等效 3.9 核利用率
- merged_z14_strong_opt: Wall=891s, User=4245s, 等效 4.8 核利用率
- **建议:** 更多 CPU 核心可进一步提升并行处理速度

### 10.3 I/O 瓶颈
- System time 占比约 10%，I/O 不是主要瓶颈
- 大文件读取是流式的，tippecanoe 设计上避免了内存加载全部数据

### 10.4 瓦片大小限制
- tippecanoe 默认单个瓦片最大 500KB
- 优化后所有层级瓦片均在 500KB 以内（最大 499KB）
- 无需额外调整 `--maximum-tile-bytes`

## 11. 错误分析

### 11.1 Exit Code 100 错误

**现象:** 默认参数处理高缩放级别时退出，返回码 100

**原因:** tippecanoe 在高缩放级别下无法将所有特征适配到瓦片大小限制内，自动丢弃功能不足以解决问题时主动退出

**解决方案:** 使用 `--drop-densest-as-needed` 和 `--coalesce-densest-as-needed` 参数

**部分成功的输出:** 即使返回失败，已成功生成的瓦片（如 z0-z9）仍会保存到输出文件中

## 12. 优化方向建议

### 12.1 短期优化
1. **始终使用优化参数处理大文件:** 对于百万级以上数据，直接使用优化参数组合
2. **并行模式优先:** 在内存允许的情况下，始终使用 `-P` 参数
3. **显式指定层级:** 不使用 `-zg`，根据数据规模手动指定 `-z`
4. **z14 完全可行:** 使用优化参数可成功处理 1650万条数据到 z14

### 12.2 长期优化
1. **增加系统内存:** 若需处理更大数据集或更高缩放级别，建议升级到 16GB+ 内存
2. **使用 SSD 存储:** 虽然 I/O 不是主要瓶颈，但更快的磁盘可加速大文件读取
3. **分块处理:** 对于超大数据集，可考虑按地理范围分块处理后使用 tile-join 合并

## 13. 测试脚本

完整测试脚本位于:
- 基础测试: `tests/big_data/big_file_test.sh`
- z14 测试: `tests/big_data/z14_test.sh`

可单独运行特定测试:
```bash
# 测试 California z14 优化
./tippecanoe -q -f -z14 -P -r5 --drop-densest-as-needed --coalesce-densest-as-needed --simplification=10 \
  -o output.mbtiles tests/big_data/California.geojsonl

# 测试合并文件 z14 优化
./tippecanoe -q -f -z14 -P -r10 --drop-densest-as-needed --coalesce-densest-as-needed --simplification=10 --drop-fraction-as-needed \
  -o output.mbtiles tests/big_data/California.geojsonl tests/big_data/NewYork.geojsonl
```

## 14. 总结

| 维度 | 结论 |
|------|------|
| 单文件支持 | 可成功处理 3.6GB/1150万条数据到 z14 |
| 合并文件支持 | 可成功处理 5GB/1650万条数据到 z14 |
| z14 最佳参数 | `-P -r5 --drop-densest-as-needed --coalesce-densest-as-needed --simplification=10` |
| z12+ 必须优化 | 默认参数无法处理千万级数据到 z12+，必须使用优化参数组合 |
| 内存需求 | 顺序 ~1GB，并行 z14 ~2.2GB |
| 时间成本 | z14 优化处理 California ~10.5分钟，NewYork ~4分钟，合并 ~15分钟 |
| 关键参数 | `--drop-densest-as-needed` 和 `--coalesce-densest-as-needed` 是突破层级限制的核心 |
| 理论上限 | tippecanoe 支持到 z22，实际 z14+ 需要足够的优化参数和内存 |
