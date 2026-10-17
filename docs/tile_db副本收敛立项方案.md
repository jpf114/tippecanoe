# 立项方案：tile.cpp / tile-db.cpp 双份瓦片循环副本收敛

> 状态：**立项待批**（本文档只做方案，不含实现）。
> 起草：2026-10-17，基于 feature/combined @ 5054408 的实测 diff。
> 背景：《架构复核》2.4 与遗留事项审计确认的最大的剩余复制债——
> 比 maindb 选项解析层（已于 5054408 收敛）范围更大、风险更高。

## 一、现状量化

| 文件 | 行数 | 角色 |
|---|---|---|
| tile.cpp | 3597 | tippecanoe 的瓦片循环（链接 tile.o，含 checkpoint.o） |
| tile-db.cpp | 3463 | tippecanoe-db 的瓦片循环（链接 tile-db.o，含 mongo.o/postgis.o） |

两文件为 db 分支 fork 时的整份复制（为引入 MongoDB 写出与 PostGIS 计时）。
实测 diff（忽略行尾空白）：**316 行变更、30 个差异块**，两文件 ~90% 相同。
每轮上游/定制演进需双份维护；本轮选项重构已实证"单边遗漏"风险
（--output-projection 曾只加到 main 的瓦片循环侧）。

## 二、差异分类（30 块的实测归类）

| 类别 | 块数 | 内容 | 收敛含义 |
|---|---|---|---|
| **Mongo 写出** | 4 | tile-db 的 `write_tile_outputs` 在瓦片写出点插入 MongoDB 批量写；tile.cpp 无 | 核心的合理差异，需要抽象 |
| **checkpoint 能力** | 8 | tile.cpp 的 traverse_zooms 多出 `checkpoint::Session*`、`initial_compressed`、`initial_skip_children` 形参与恢复逻辑；tile-db 无 | tile-db 不需要（db 无断点续做），参数默认值即可兼容 |
| **drop-by-attribute** | 7 | `choose_minattribute` 函数及瓦片内丢弃调用；tile-db 无 | 同上，能力参数（指针/bool）默认禁用即可 |
| **其它微差** | 11（72 行） | include 集合、`std::atomic_init` vs 构造初始化（gcc 兼容写法）、`1 <<` vs `1U <<`、`maindb.hpp`/`error_logger.hpp` 头 | 其中可能埋着**单边修复未回流**（atomic_init 风格差异即疑似），收敛时顺带消除 |

结论：差异的 86%（25/30 块）是"能力有无"（一方有另一方没有），只有 Mongo
写出是真正的行为分叉。这意味着收敛的抽象面很窄。

## 三、方案比选

### 方案 A：单份源 + 写出端 hook + 能力参数复用（推荐）

- `tile.cpp` 成为唯一实现。Mongo 写出点改为函数指针 hook
  （先例：checkpoint M13 的 `set_json_read_shutdown_hook`；以及本项目已有的
  `encode_index`/`unproject_output_ll` 模式）：
  `void (*tile_output_sink)(int z, unsigned x, unsigned y, const char *data, size_t len) = NULL;`
  默认 NULL → 走现有 mbtiles/dir 直写（tippecanoe 行为零变化）；
  `tippecanoe-db` 启动时注入 Mongo+mbtiles 双写实现（实现在 tile-db.cpp 或
  mongo_manager，链接期隔离 mongo 符号）。
- `traverse_zooms` 的 checkpoint/drop-by-attribute 参数已有默认值或可加默认值
  （`session=NULL`、`drop_by_attribute=nullptr/false`），tippecanoe-db 直接调用
  并传禁用值——**能力差异从"代码副本"降级为"参数组合"**。
- tile-db.cpp 缩为薄壳：hook 注入 + PostGIS 计时 + `maindb.hpp` 等自身依赖。
- 优点：彻底消灭双份；未来瓦片引擎改动单点生效；顺带回流 atomic_init 等
  单边微修。
- 缺点/风险：hook 在热路径（每瓦片一次），需确认无性能回退（一次空指针
  判断，可忽略）；Mongo 写出点共 4 处，需逐一圈定调用面（含 coalesce 时的
  提前写出路径）；`tile-db.o` 的特殊编译规则（mongo.hpp include 路径）需要
  保留给注入实现文件。

### 方案 B：条件编译（#ifdef TIPPECANOE_DB）

一份源文件，差异块用宏分支。收敛彻底、改动量最小，但源文件内长期宏分支
是新的丑债，且两目标共享同一 .o 后编译依赖复杂化（mongo.hpp 路径污染
tippecanoe 目标）。**仅作为方案 A 遇阻时的兜底**。

### 方案 C：保持副本 + 同步守护脚本

不做结构改动，用 `diff` 漂移检查（差异块白名单）+ PR 检查清单。成本最低，
但复制债本身不消失，不推荐作为终态；可作为方案 A 落地前的过渡护栏。

## 四、分步计划（方案 A）

| 步骤 | 内容 | 验证 |
|---|---|---|
| 1 | 精确圈定 Mongo 写出调用面：4 个差异块的函数、调用频率（含 coalesce 提前写出）、数据流（compressed buffer？） | 输出调用面清单，评审 |
| 2 | tile.cpp 引入 `tile_output_sink` hook 与能力参数默认值；tile.cpp 现有调用点改为 hook 判空直写 | tippecanoe 编译 + make test 全绿（重点 pbf/accumulate/checkpoint） |
| 3 | tile-db.cpp 改薄壳：注入双写 sink（Mongo 统计/批量/重试逻辑原样搬运）、traverse_zooms 改调 tile.cpp 版（checkpoint/drop-by-attribute 传禁用值）；删除重复主体 | tippecanoe-db 编译 |
| 4 | 回归矩阵：tests/qa 全套（run_p1db/run_m15/run_options_refactor）+ china/gc 基准 md5 + E11000 幂等 + 性能复测（10 万要素对比 2.8s 基线，容差 ±10%） | 全绿 |
| 5 | 清理：Makefile 双目标对象表更新、单边微修回流确认（atomic_init 等 11 块逐块裁决） | diff 归零，Makefile 简化 |
| 6 | 文档：README/架构复核标注副本已收敛；CHANGELOG 补条目 | — |

## 五、验收标准（DoD）

1. `tile-db.cpp` 不再包含瓦片循环主体（目标 <200 行，仅为注入与 db 专属逻辑）；
   瓦片循环源文件唯一。
2. `make test` 全绿；tests/qa 全套通过；china/gc 基准字节级一致；
   E11000 幂等；性能无回退（±10% 内）。
3. checkpoint 能力（tippecanoe）与 Mongo 能力（tippecanoe-db）行为与收敛前
   逐字节一致。
4. unit 增加防回归用例：两个目标的 traverse_zooms 符号来源唯一性
   （链接检查）或等价手段。

## 六、工作量与时机

- 估算：2~3 人日（步骤 2-3 是主体；步骤 1 的调用面圈定是风险集中点——
  Mongo 写出涉及压缩 buffer 生命周期，需谨慎）。
- 时机建议：**当前功能冻结窗口内做**（next 功能开发前），因为收敛后再做
  引擎级改动只需单点维护；若近期有紧急业务需求，可先落方案 C 过渡。
- 依赖：无（checkpoint/Mongo 接口均已稳定，质检 P0-P2 已闭环）。

## 七、开放问题（实施时裁决）

1. Mongo 写出是否可与 mbtiles 写出共用同一压缩 buffer（当前 tile-db 的
   d 字段 binary 与 mbtiles 的 tile_data 同源 gzip？需确认是否二次压缩）。
2. `write_tile_outputs` 的 coalesce 提前写出路径是否对 sink 语义有
   "一瓦片多次调用"的假设——hook 契约需明确幂等/顺序要求。
3. PostGIS 计时/进度日志（tile-db 特有 profile 输出）归入 sink 侧还是
   循环侧——建议循环侧保留计时，sink 侧只管写出。
