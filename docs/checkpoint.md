# Tippecanoe 断点续做（Checkpoint / Resume）

本文档说明原生 `tippecanoe`（GeoJSON / CSV 等读入路径）的 **zoom 级断点续做** 功能。  
**不包含** `tippecanoe-db`（PostGIS / Mongo 等），后者在后续版本中再考虑。

---

## 1. 功能概述

在生成矢量瓦片时，任务往往耗时很长（读入大 GeoJSON、多级 zoom 切片）。进程被 `kill -9`、OOM 或机器重启打断后，传统做法只能从头重跑。

断点续做提供：

- **专用 checkpoint 工作目录**（与 `-o` / `-e` 输出路径分开）；
- **每个 zoom 成功结束后** 落盘中间状态；
- **`--resume`** 时跳过读入与 radix，从「上一完成 zoom 的下一级」继续切片。

### 1.1 两阶段模型（必读）

| 阶段 | 内容 | 是否 checkpoint |
|------|------|-----------------|
| **阶段 A** | 读入 GeoJSON、radix 排序、合并 stringpool、建 shared_nodes 等 | **否** |
| **阶段 B** | `traverse_zooms`：按 zoom 写 mbtiles 或目录瓦片 | **是**（每 zoom 一次） |

因此：

- 中断发生在 **读入 / 排序** 时：checkpoint 目录里通常还没有可用的「切片入口快照」，下次只能 **全量重跑**（与计划一致）。
- 中断发生在 **切片过程中**：已完成的 zoom 有记录，且已保存进入下一 zoom 所需的 geom 分片与 stringpool，可 **`--resume` 续跑**。

「zoom 级」**不是**「只记录 mbtiles 里已有哪些 z」，而是必须保存 **算法中间态**（否则无法从 z+1 继续，因为 z+1 的输入来自 z 结束时生成的 geom 分片，且属性索引依赖同一次读入的 stringpool）。

### 1.2 流程示意

```
读入 GeoJSON → radix / 合并 pool → [snapshot_tiling_entry] → zoom 0 → commit
                                                              → zoom 1 → commit
                                                              → ...
                                                              → 完成
```

续跑时：

```
--resume → 校验 fingerprint → restore（mmap pool、geom 分片、skip_children 等）
         → 跳过阶段 A → traverse_zooms(iz = last_completed_zoom + 1)
```

---

## 2. 设计说明

### 2.1 为什么用「目录 + SQLite」，而不是把大文件放进数据库？

| 内容 | 存储位置 | 原因 |
|------|----------|------|
| 作业指纹、命令行、zoom 进度、时间戳 | `state.sqlite` | 便于事务、查询、原子更新 |
| stringpool、geom 分片、shared_nodes、bloom | `blobs/` 下普通文件 | 可达 GB 级；SQLite BLOB 性能差、单文件易膨胀 |
| 写入中的数据 | `staging/` | 写完后 `rename` 到 `blobs/`，再更新 SQLite |

SQLite 充当 **作业账本**；文件系统充当 **中间态仓库**。

### 2.2 提交顺序（防止半截状态）

每个 zoom 的 `commit` 顺序为：

1. 将新的 geom 分片、`skip_children`、`strategies` 写入 `staging/`；
2. 对相关文件 `fsync`；
3. `rename` 到 `blobs/`；
4. SQLite 事务：写入 `zoom_commit`，更新 `tiling_state.last_completed_zoom` 等。

仅在当前 zoom 的 drop 多 pass 循环 **全部成功**（`again == false`）且 `err == INT_MAX` 时调用 `on_zoom_complete`，避免「重试中」的错误进度。

### 2.3 与切片算法的关系

- **不修改** `write_tile`、drop/coalesce、几何切分、radix 等核心逻辑；
- 仅在 `traverse_zooms` 循环末尾增加 **一处** 钩子，具体逻辑在 `checkpoint.cpp`；
- `main.cpp` 的 `read_input` 首尾负责「是否跳过读入」与 `snapshot_tiling_entry`。

### 2.4 作业指纹（fingerprint）

续跑前会计算 SHA-256 指纹，与首次运行时写入 `job.fingerprint` 的值比对。参与哈希的包括：

- 格式版本 `TIPPECANOE_CHECKPOINT_FORMAT`；
- **规范化后的命令行**（已剔除 `--checkpoint-dir`、`--resume`、`--checkpoint-force`、`-F`、`-f` 等与瓦片内容无关或仅影响输出的标志）；
- 输出模式（`mbtiles` / `directory` / 记录用 `pmtiles`）与 **输出路径绝对路径**；
- `TEMP_FILES`、`CPUS`；
- `prevent[]` / `additional[]` 标志位；
- 输入文件列表：`绝对路径 + size + mtime`（排序后）。

**任一影响瓦片内容的参数或输入文件变化**，续跑会以退出码 `101`（`EXIT_ARGS`）失败并打印 stored / expected 指纹。

续跑时仍须在命令行传入输入文件路径，用于校验；**不会**再执行读入与 radix（当存在有效切片 checkpoint 时）。

---

## 3. Checkpoint 目录结构

用户通过 `--checkpoint-dir=/path/to/job` 指定目录（不要与 `-o` / `-e` 混用同一目录）：

```
job/
  state.sqlite              # 元数据与进度（WAL 模式）
  blobs/
    stringpool              # 进入切片后的 pool 快照（pool 非空时才有）
    geom.0 .. geom.N-1      # 下一 zoom 的输入分片（N = TEMP_FILES）
    shared_nodes            # 启用 shared borders 且 nodepos > 0 时
    shared_nodes.bloom
    pool_off.bin            # 各读入线程在 pool 中的偏移
    initial_x.bin
    initial_y.bin
    layermaps.bin           # 图层 id 等切片所需快照
    skip_children.bin       # 每个 zoom 提交后更新
    strategies.bin
  staging/                  # 写入临时文件，提交后 rename 到 blobs/
```

### 3.1 SQLite 表（schema v1）

- **`job`**：指纹、命令行、输出模式与路径、`temp_files`、`cpus`、时间戳等（单行 `id=1`）。
- **`input_file`**：各输入文件 path / size / mtime。
- **`tiling_state`**：`iz`、`minzoom`、`maxzoom`、`basezoom`、`last_completed_zoom`、`midx`/`midy`、`entry_snapshot_done` 等。
- **`zoom_commit`**：每个已提交 zoom 的状态、`geom_total_bytes`、`generation`。

---

## 4. 命令行参数

| 参数 | 说明 |
|------|------|
| `--checkpoint-dir=DIR` | 启用 checkpoint，元数据与 blobs 写入 `DIR` |
| `--resume=DIR` | 从已有 checkpoint 目录续跑；**与 `--checkpoint-dir` 互斥** |
| `--checkpoint-force` | 若 `DIR` 中已有作业，清空后重新开始（仅影响 checkpoint 目录，不是 `-f`） |

### 4.1 与现有参数的配合

| 场景 | 要求 |
|------|------|
| 首次跑 + checkpoint | `--checkpoint-dir=DIR` + `-o out.mbtiles` 或 `-e outdir` |
| 续跑 | `--resume=DIR` + **相同** `-o`/`-e` 与 **相同** 切片相关参数（如 `-z`、`-Z`、`-d` 等）+ **必须** `-F` / `--allow-existing` |
| 清空输出重来 | 对输出使用 `-f`；对 checkpoint 使用 `--checkpoint-force` |
| 无 checkpoint 参数 | 行为与未加此功能前的 tippecanoe **完全一致** |

### 4.2 输出格式说明

| 输出 | 首次 `--checkpoint-dir` | `--resume` |
|------|-------------------------|------------|
| mbtiles（`-o *.mbtiles`） | 支持 | 支持 |
| 目录（`-e DIR`） | 支持 | 支持 |
| PMTiles（`-o *.pmtiles`） | 可记录指纹 | **不支持**，会报错并提示先用 mbtiles/目录，再按现有流程转 PMTiles |

---

## 5. 使用方法

### 5.1 首次运行（启用 checkpoint）

```bash
tippecanoe -zg \
  -o /data/out.mbtiles \
  --checkpoint-dir=/data/job-42 \
  /data/in.geojson
```

说明：

- checkpoint 目录可以事先不存在，会自动创建；
- 若目录里 **已有** 未清除的作业（存在 `state.sqlite` 中的 job 记录），需加 `--checkpoint-force` 或换目录。

### 5.2 中断后续跑

```bash
tippecanoe -z14 -Z0 \
  --resume=/data/job-42 \
  -o /data/out.mbtiles \
  -F \
  /data/in.geojson
```

**注意：**

1. **`-z`、`-Z`、`-d` 等影响瓦片的参数必须与首次运行一致**（不要首次无 `-q`、续跑加 `-q`，会导致指纹不一致）。
2. **必须加 `-F`**，否则无法保留已写入的瓦片。
3. 输入文件路径仍需传入，用于 fingerprint；有有效 checkpoint 时 **不会** 再读 GeoJSON。

### 5.3 清空 checkpoint 后重新开作业

```bash
tippecanoe --checkpoint-dir=/data/job-42 --checkpoint-force \
  -o /data/out.mbtiles -f \
  /data/in.geojson
```

- `-f`：删除或覆盖 **输出** mbtiles；
- `--checkpoint-force`：清空 **checkpoint 目录** 内旧作业。

### 5.4 目录输出示例

```bash
tippecanoe -z10 -e /data/tiles \
  --checkpoint-dir=/data/job-tiles \
  china.geojson

# 续跑
tippecanoe -z10 -e /data/tiles \
  --resume=/data/job-tiles \
  -F china.geojson
```

### 5.5 投影（CRS）提示

若 GeoJSON 内声明的 CRS 与 tippecanoe 默认不一致（例如 EPSG:4490），日志会有警告。这不影响 checkpoint 机制本身；若需指定投影，加上 `-s` 等参数，且 **首次与续跑须一致**（会进入 fingerprint）。

---

## 6. 行为与退出码

| 情况 | 行为 |
|------|------|
| 切片未跑完所有 zoom | 与原先一致，可能返回 `100`（`EXIT_INCOMPLETE`）；checkpoint 中记录 `last_completed_zoom`，续跑从下一级补 |
| 指纹不一致 | 退出 `101`，打印 stored / expected |
| 续跑未加 `-F` | 启动阶段报错退出 |
| 同时指定 `--checkpoint-dir` 与 `--resume` | 报错退出 |
| 读入阶段中断且无 entry snapshot | `--resume` 无法跳过读入，相当于无切片 checkpoint，需全量重跑 |

续跑完成后若输出里已有 metadata，可能出现 `UNIQUE constraint failed: metadata.name` 等提示，一般可忽略（正在更新已有 tileset 的元数据）。

---

## 7. 故障排查

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| `checkpoint fingerprint mismatch` | 续跑时 `-z`、输入文件或输出路径与首次不同 | 使用与首次完全相同的切片参数与输入；检查是否多了 `-q` |
| `checkpoint directory already has a job` | 目录里已有作业 | `--checkpoint-force` 或换新 `--checkpoint-dir` |
| `--resume requires -F` | 未保留已有瓦片 | 加上 `-F` |
| `--resume is not supported for PMTiles` | 输出为 PMTiles | 改用 mbtiles 或 `-e` 目录 |
| `did not close all files`（旧版本） | checkpoint 的 SQLite 未关闭 | 请使用当前分支；进程结束前会关闭 `state.sqlite` |
| 续跑很快结束、瓦片未增加 | 首次已跑完全部 zoom | 正常；检查 `last_completed_zoom` 是否已达 `maxzoom` |

查看进度示例：

```bash
sqlite3 /data/job-42/state.sqlite \
  "SELECT last_completed_zoom, entry_snapshot_done FROM tiling_state;"
sqlite3 /data/job-42/state.sqlite \
  "SELECT zoom, committed_at FROM zoom_commit ORDER BY zoom;"
```

---

## 8. 测试

```bash
make tippecanoe
make checkpoint-test
```

`checkpoint-test` 覆盖：基本 checkpoint/resume、fingerprint 拒绝、目录输出、kill -9 后续跑（小数据集）。

---

## 9. 限制与后续规划

### 9.1 当前限制

- 仅 **原生** `main.cpp` / `tile.cpp` 路径，不含 `tippecanoe-db`；
- **读入过程**无增量 checkpoint；
- checkpoint 磁盘占用约为 **stringpool + geom 分片** 量级，请把 `--checkpoint-dir` 放在空间充足的磁盘；
- 不支持 `--checkpoint-prune`（成功后自动删 blobs）、`--checkpoint-trust-output`（从输出反推进度）等专家选项。

### 9.2 后续可能增加

- 读入阶段按文件/块增量 checkpoint；
- PMTiles 可续写或完成后一键转换；
- `tippecanoe-db` 共用 checkpoint 库；
- `SIGTERM` 时优雅提交当前 zoom；
- `--checkpoint-prune` 等维护命令。

---

## 10. 相关源码（便于维护）

| 文件 | 职责 |
|------|------|
| [`checkpoint.hpp`](../checkpoint.hpp) / [`checkpoint.cpp`](../checkpoint.cpp) | Session、fingerprint、SQLite、blobs、序列化 |
| [`main.cpp`](../main.cpp) | CLI、`read_input` 挂接 |
| [`tile.cpp`](../tile.cpp) | `traverse_zooms` 末尾 `on_zoom_complete` |
| [`Makefile`](../Makefile) | 链接 `checkpoint.o`，目标 `checkpoint-test` |
