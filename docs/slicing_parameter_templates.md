# 切片参数配置模板

## 参数速查表

| 参数 | 短选项 | 默认值 | 含义 |
|------|--------|--------|------|
| `-r` (drop-rate) | -r | 2.5 | 低缩放级别特征丢弃率 |
| `--drop-densest-as-needed` | 无 | 关 | 丢弃密集区域特征 |
| `--coalesce-densest-as-needed` | 无 | 关 | 合并密集区域特征 |
| `--simplification` | -S | 1 | 几何简化程度 |
| `--drop-fraction-as-needed` | 无 | 关 | 按比例丢弃特征 |
| `--maximum-tile-bytes` | -M | 500000 | 单瓦片最大字节数 |
| `--maximum-tile-features` | -O | 200000 | 单瓦片最大特征数 |
| `--postgis-shard-key` | 无 | 无 | 并行读取分片列 |
| `--postgis-shard-mode` | 无 | auto | 并行读取分片模式 |

## 标准配置模板

### 模板 1: 小数据 Point (≤ 5K)

```bash
tippecanoe-db -q -f -z12 \
  --postgis "host:port:dbname:user:password:table:geom" \
  --mongo "host:port:dbname:user:password:auth_source:collection"
```

### 模板 2: 中等数据 Point (5K-50K)

```bash
tippecanoe-db -q -f -z14 \
  -r5 --drop-densest-as-needed --coalesce-densest-as-needed \
  --postgis "host:port:dbname:user:password:table:geom" \
  --postgis-shard-key "ogc_fid" --postgis-shard-mode "key" \
  --mongo "host:port:dbname:user:password:auth_source:collection"
```

### 模板 3: 大数据 Point (50K-500K)

```bash
tippecanoe-db -q -f -z14 \
  -r5 --drop-densest-as-needed --coalesce-densest-as-needed --simplification=10 \
  --postgis "host:port:dbname:user:password:table:geom" \
  --postgis-shard-key "ogc_fid" --postgis-shard-mode "key" \
  --mongo "host:port:dbname:user:password:auth_source:collection"
```

### 模板 4: 超大数据 Point (500K+)

```bash
tippecanoe-db -q -f -z14 \
  -r10 --drop-densest-as-needed --drop-fraction-as-needed --coalesce-densest-as-needed \
  --simplification=10 --maximum-tile-bytes=512000 \
  --postgis "host:port:dbname:user:password:table:geom" \
  --postgis-shard-key "ogc_fid" --postgis-shard-mode "key" \
  --mongo "host:port:dbname:user:password:auth_source:collection"
```

### 模板 5: 中等数据 Polygon (≤ 10K)

```bash
tippecanoe-db -q -f -z12 \
  -r5 --drop-densest-as-needed --coalesce-densest-as-needed --simplification=10 \
  --postgis "host:port:dbname:user:password:table:geom" \
  --postgis-shard-key "ogc_fid" --postgis-shard-mode "key" \
  --mongo "host:port:dbname:user:password:auth_source:collection"
```

### 模板 6: 大数据 Polygon (10K+)

```bash
tippecanoe-db -q -f -z12 \
  -r10 --drop-densest-as-needed --drop-fraction-as-needed --coalesce-densest-as-needed \
  --simplification=20 --maximum-tile-bytes=256000 \
  --postgis "host:port:dbname:user:password:table:geom" \
  --postgis-shard-key "ogc_fid" --postgis-shard-mode "key" \
  --mongo "host:port:dbname:user:password:auth_source:collection"
```

> ⚠️ Polygon 数据不建议使用 z14+，除非业务必需。8K个Polygon在z14可产生200万+瓦片。

## 决策树

```
数据量 < 5K?
  ├─ Point → 模板1 (z12, 默认参数)
  └─ Polygon → 模板5 (z12, 优化参数)

数据量 5K-50K?
  ├─ Point → 模板2 (z14, -r5 + drop-densest)
  └─ Polygon → 模板5 (z12, 优化参数)

数据量 50K-500K?
  ├─ Point → 模板3 (z14, -r5 + simplification=10)
  └─ Polygon → 模板6 (z12, -r10 + simplification=20)

数据量 > 500K?
  ├─ Point → 模板4 (z14, -r10 + 全优化)
  └─ Polygon → 模板6 (z12, -r10 + 全优化)
```

## 参数详解

### -r (drop-rate)

低缩放级别的特征丢弃率。`-r5` 表示每降一级保留 1/5 的特征：

```
z14: 保留全部 (interval = 5^0 = 1)
z13: 每5个保留1个 (interval = 5^1 = 5)
z12: 每25个保留1个 (interval = 5^2 = 25)
z8:  每15625个保留1个 (interval = 5^6 = 15625)
z5:  每1953125个保留1个 (interval = 5^9 = 1953125)
```

### --drop-densest-as-needed

瓦片超限时自动丢弃最密集区域的特征，保留稀疏区域。这是突破层级限制的核心参数。

算法：
1. 计算瓦片内所有特征之间的间隔(gap)
2. 对 gap 排序，选择第 (1-f) 分位数处的 gap 值
3. 只保留 gap 大于等于该阈值的特征
4. 每次迭代 f 按 `max_size/current_size * 0.80` 缩小

### --coalesce-densest-as-needed

与 drop-densest 类似，但将密集特征合并而非丢弃，保留更多属性信息。通常与 drop-densest 同时使用。

### --simplification

几何简化程度，值越大顶点越少。默认值 1 表示轻度简化，设为 10 可显著减少多边形顶点数。Polygon 数据尤其需要较高的简化值。

### --postgis-shard-key / --postgis-shard-mode

并行读取配置。推荐使用 `--postgis-shard-key=ogc_fid --postgis-shard-mode=key`。

| 模式 | 适用场景 | 需要 shard_key | 当前可用性 |
|------|---------|---------------|-----------|
| auto | 默认，自动尝试 | 否 | ❌ ctid在子查询中不可用 |
| key | 任意列哈希分片 | **是** | ✅ 推荐 |
| range | 整数列范围分片 | 是(整数) | ❌ CAST在子查询中失败 |
| none | 禁用分片，单线程 | 否 | ✅ 可用但无加速 |

### exit code 100 处理

如果切片以 exit code 100 退出，说明瓦片超限。解决方法：

1. 添加 `--drop-densest-as-needed --coalesce-densest-as-needed`
2. 增大 `-r` 值（如 -r5 → -r10）
3. 增大 `--simplification` 值（如 10 → 20）
4. 降低最大缩放级别（如 -z14 → -z12）
5. 添加 `--drop-fraction-as-needed`（最激进）
