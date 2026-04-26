# Tippecanoe-DB 参数收敛与架构收口实施计划

> 用于后续实施跟踪。以小步修改、随改随验为原则，避免一次性大改。

## 目标

本轮改造只聚焦 4 件事：

1. 收敛 `tippecanoe-db` 对外参数，只保留普通用户真正需要的核心参数。
2. 为复杂参数提供稳定默认值，同时保留高级覆盖能力。
3. 收口 `PostGIS -> Tippecanoe -> Mongo/MBTiles` 的职责边界，降低后续维护成本。
4. 明确 `Mongo` 为业务输出、`MBTiles` 为校验输出的语义，保留结果一致性验证路径。

## 范围边界

本轮要做：

- 调整 `tippecanoe-db` 的参数分层与默认值策略
- 整理主入口中的参数解析、配置归一化、输出模式判断
- 小范围收口 `PostGIS` 输入层和 `Mongo` 输出层的职责
- 更新帮助信息与关键错误提示
- 增加最小必要测试，防止参数与输出语义回归

本轮不做：

- 不重写 Tippecanoe 原生切片核心
- 不做大规模文件拆分
- 不改原生 `tippecanoe` 程序入口
- 不同步修改 README / docs 说明，除非后续明确要求
- 不一次性解决所有深层架构问题，只处理影响可用性和维护性的部分

## 当前判断

- 当前定制主入口在 `maindb.cpp`，而不是原生 `main.cpp`
- 当前参数较多，普通用户需要面对过多“内部控制旋钮”
- 当前 `Mongo` 与 `MBTiles` 的输出角色在代码语义上还不够清晰
- 当前 `PostGIS` 输入层承担了较多自动推断与内部策略逻辑
- 当前工作区已有 `postgis.cpp` 未提交改动，实施时必须谨慎避免覆盖

## 参数分层方案

### 一类：核心参数

主帮助中保留，面向普通用户：

- `--postgis`
- `--postgis-table`
- `--postgis-sql`
- `--postgis-geometry-field`
- `--mongo`
- `-o`
- `-z`
- `-Z`
- `--force`

说明：

- `-l/--layer` 当前在原生 `main.cpp` 中存在，但 `tippecanoe-db` 入口 `maindb.cpp` 里尚未完整暴露。
- 本轮实施中需要先确认是否将 `layer` 参数补入 `tippecanoe-db`，再决定是否把它纳入最终核心参数集合。

### 二类：高级参数

继续支持，但不放在主帮助主路径：

- `--postgis-columns`
- `--mongo-metadata`
- `--mongo-drop-collection`
- `--mongo-no-indexes`
- `--mongo-no-fail-on-discard`

### 三类：专家参数

保留兼容，但默认隐藏，仅用于调优和排障：

- `--postgis-shard-mode`
- `--postgis-shard-key`
- `--postgis-progress-count`
- `--postgis-canonical-attr-order`
- `--postgis-no-canonical-attr-order`
- `--postgis-profile`
- `--mongo-batch-size`
- `--mongo-pool-size`
- `--mongo-timeout`

## 默认值策略

### PostGIS 默认策略

- 默认开启 cursor
- 默认不开精确 count
- 默认不开 profile
- 默认保留源字段顺序，以便 MBTiles 校验结果默认贴近原生 tippecanoe
- 默认不要求用户显式配置分片参数
- 默认优先支持表输入，SQL 输入作为高级用法继续保留

### Mongo 默认策略

- 默认创建索引
- 默认 `fail_on_discard = true`
- 默认自动 batch 策略
- 默认使用内部 pool size / timeout
- 默认写 metadata

### 输出模式默认语义

- `--mongo`：业务输出
- `-o`：校验输出，可选但建议保留
- 同时指定 `--mongo` 和 `-o`：业务输出 + 校验基线
- 只指定 `-o`：本地验证模式
- 只指定 `--mongo`：直接业务落库，但应明确提示未启用 MBTiles 校验

## 预计修改文件

核心文件：

- `maindb.cpp`
- `maindb.hpp`
- `postgis.cpp`
- `postgis.hpp`
- `postgis_manager.cpp`
- `mongo.cpp`
- `mongo.hpp`
- `mongo_manager.cpp`
- `tile-db.cpp`
- `config.hpp`

可能涉及的测试文件：

- `unit.cpp`
- `tests/` 下现有相关测试文件

原则上不动：

- `main.cpp`
- 原生 `tippecanoe` 主流程文件

## 实施阶段

### 阶段 1：固定产品语义

目标：先把程序怎么用、参数如何分层、输出语义是什么固定下来。

任务：

- [x] 明确 `tippecanoe-db` 的对外定位为“PostGIS 输入 + Mongo 业务输出 + MBTiles 可选校验输出”
- [x] 固定核心参数集合
- [x] 纠正计划与代码现实的偏差：确认 `tippecanoe-db` 是否在本轮补充 `layer` 参数入口
- [x] 固定高级参数集合
- [x] 固定专家参数集合
- [x] 明确参数帮助展示分层策略

完成标准：

- 参数分层规则明确
- 后续代码改造不再围绕参数含义反复返工

### 阶段 2：整理参数解析与配置归一化

目标：把“识别参数”和“归一化运行配置”分开。

任务：

- [x] 梳理 `maindb.cpp` 中 PostGIS 相关参数解析
- [x] 梳理 `maindb.cpp` 中 Mongo 相关参数解析
- [x] 增加统一配置归一化步骤
- [x] 将默认值策略从分散逻辑收口到统一位置
- [x] 保留旧参数兼容，但不再作为主帮助主路径

当前进展：

- 已新增 `db_options.hpp`，把核心帮助参数判断与运行默认值归一化抽成可复用逻辑
- `maindb.cpp` 已接入 `db_options.hpp`
- `maindb.cpp` 中 PostGIS / Mongo 参数解析已收口为独立 helper，主循环现在只负责分发
- `--postgis` / `--mongo` 已支持“短连接串优先、旧格式兼容”
- Mongo metadata 默认开启，但已补充 `--mongo-no-metadata` 作为显式覆盖入口
- PostGIS / Mongo 的部分运行默认值已开始从构造期/运行期对象内迁回统一 `normalize()` 阶段
- 旧参数兼容仍保留，但已移出主帮助发现路径
- 当前 `unit` / `make tippecanoe-db` 已可作为稳定验证手段

完成标准：

- 参数解析逻辑更清晰
- 默认值行为集中、可追踪

### 阶段 3：收口 PostGIS 输入层边界

目标：让输入层以“稳定读取”为主，减少过度聪明的自动行为扩散。

任务：

- [x] 重新梳理 `postgis.hpp` 中配置字段的职责边界
- [x] 明确哪些配置属于用户输入，哪些属于内部策略
- [x] 收紧分片相关默认策略
- [x] 收敛不应暴露给普通用户的输入层参数
- [x] 保持现有主链路不被破坏

当前进展：

- `postgis_config` 已逐步内聚输入源语义：
  `has_sql_input / has_table_input / has_input_source / sql_takes_precedence / effective_layer_name`
- `postgis_config` 已开始内聚默认策略语义：
  `geometry_field_uses_default / uses_default_shard_mode / effective_shard_mode`
- `postgis_config` 已补充“是否偏离默认运行策略”的判断：
  `has_read_tuning_overrides / has_attribute_strategy_overrides / has_debug_strategy_overrides`
- `postgis_config` 已补充更细粒度的默认值判断，
  例如 batch/cursor/memory/retries/progress/attribute-order/exact-count/profile
- `postgis_config` 已补充若干输入约束规则判断，
  例如 `requires_selected_columns_for_best_effort / requires_shard_key / ignores_shard_key`
- `postgis_config` 现在也负责判断哪些输入细节应在运行摘要中显示，
  减少 `maindb.cpp` / `postgis_manager.cpp` 自己拼装默认策略判断
- `PostGIS::validate_config(...)` 已开始接管更多配置合法性校验，
  例如 best-effort columns / shard-key 这类硬规则，不再只散落在 `maindb.cpp`
- `postgis.cpp` 中实现层对默认分片模式的重复推断已开始收口到 `effective_shard_mode()`
- `postgis_config` 已继续内聚分片模式语义：
  `is_supported_shard_mode / has_supported_shard_mode / is_auto_shard_mode / is_none_shard_mode / is_key_shard_mode / is_range_shard_mode`
- `maindb.cpp` 中 `--postgis-shard-mode` 的合法值校验已回收到 `postgis_config`，避免入口层继续散落 `auto|none|key|range` 字符串判断
- `postgis.cpp` 的 `prepare_shard_plan(...)` 已统一依赖 `postgis_config` 分片 helper，
  现在“是否可尝试 range/key/ctid sharding”的认知只保留一处
- `postgis_manager.cpp` 的并发读取摘要已改为“默认只报线程数，非默认分片策略才追加细节”
- `postgis.cpp` 的 progress / cursor / retry / thread summary 日志现在已统一受 `enable_progress_report` 控制
- `maindb.cpp` 的 PostGIS 运行摘要现在也会只在偏离默认策略时追加 read/attrs/debug 细节
- `postgis-canonical-attr-order / postgis-no-canonical-attr-order / postgis-profile / postgis-progress-count`
  已降为“兼容但不展示”的 legacy tuning flags，不再进入帮助发现路径
- `postgis.cpp` 的“分片失败回退到顺序读取”提示已收紧为只由主线程输出，避免默认 auto 模式在并行读时刷重复诊断信息
- 已验证 `./tippecanoe-db --help-advanced` 只保留 `postgis-columns-best-effort / postgis-shard-key / postgis-shard-mode`
  这类仍值得高级用户发现的 PostGIS 参数

完成标准：

- 输入层职责更单纯
- 默认行为更稳

### 阶段 4：收口 Mongo / MBTiles 输出层边界

目标：让业务输出与校验输出的角色清楚，减少切片主流程和输出状态耦合。

任务：

- [x] 在主流程中固定 `Mongo` 与 `MBTiles` 的输出模式语义
- [x] 小范围整理 `tile-db.cpp` 中的输出接入点
- [x] 收口 Mongo 默认写入策略
- [x] 确保 Mongo 打开时不破坏 MBTiles 作为校验基线的可比性

当前进展：

- 已在 `db_options.hpp` 中引入统一 `db_output_mode`
- 已把 `Mongo 业务输出 / MBTiles 校验输出 / 目录输出` 的判断收口到统一 helper
- 已修正“本地校验模式仍被无条件 `validate_mongo_config()` 拦截”的逻辑矛盾
- 已修正 `Mongo + 目录输出(-e)` 被误判为“directory only”的输出摘要错误
- 已去掉“非 Mongo 输出模式仍执行 Mongo batch 自动估算”的职责污染
- 已把 `tile-db.cpp` 中分散的“写 tile 输出 / 擦除 zoom 输出”收口为局部 helper，减少主切片逻辑中的输出分支噪声
- 已把 `--mongo-metadata` / `--mongo-fail-on-discard` 从帮助发现路径中移除，仅保留兼容解析；因为这两个行为本来就是默认开启
- 已将 Mongo 运行摘要改为“默认只显示核心状态，偏离默认值时才追加细节”，减少普通用户面对的调参噪声
- 已补充输出模式单测，避免后续再次把本地校验路径绑回 Mongo 必选

阶段结论：

- 当前 `tippecanoe-db` 已基本形成稳定输出边界：
  `PostGIS -> Tippecanoe 生成统一压缩 tile payload -> Mongo/MBTiles/目录输出按同一 payload 分发`
- 这意味着 Mongo 业务输出不会单独走一套切片结果生成逻辑，因此不会天然破坏与 MBTiles 校验基线的可比性

完成标准：

- 输出语义稳定
- Mongo 与 MBTiles 的角色清楚

### 阶段 5：帮助信息与错误提示收口

目标：主帮助只展示普通用户需要的内容。

任务：

- [x] 改主帮助输出，只保留核心参数
- [x] 为高级/专家参数保留隐藏入口或分组展示
- [x] 调整关键错误提示，使其更贴近业务语义
- [x] 调整输出模式相关提示，避免误导

当前进展：

- 已把主帮助改为仅展示核心参数
- 已补充核心工作流说明
- 已补充 `--postgis` / `--mongo` 的短连接串格式提示
- 已补充 Mongo 业务输出 / MBTiles 校验输出的提示语义
- 已新增 `--help-advanced`，按高级参数 / 专家参数两层展示数据库相关隐藏参数
- 已补充更贴近业务语义的错误提示与运行摘要，减少“参数存在但行为不透明”的问题

完成标准：

- 用户第一次使用时无需理解内部策略参数
- 错误提示足够清楚

### 阶段 6：最小测试与回归验证

目标：做最小但关键的防回归验证，不做过度测试。

任务：

- [x] 增加参数解析与默认值归一化测试
- [x] 增加输出模式测试
- [x] 增加核心帮助信息测试
- [x] 编译验证 `tippecanoe-db`
- [x] 视环境情况做一次最小冒烟验证

当前进展：

- 已补参数分层、连接串解析、默认值归一化相关测试
- 已补输出模式与 Mongo 默认状态识别测试
- 已补核心帮助参数 / 高级帮助参数 / 专家帮助参数分层测试，避免帮助发现路径回归
- 已多轮验证 `make unit`、`./unit "[db-options]"`、`./unit "[postgis]"`、`./unit`
- 已多轮验证 `make tippecanoe-db`
- 已验证 `./tippecanoe-db --help` 不再暴露 Mongo 高级参数
- 已验证 `./tippecanoe-db --help-advanced` 只显示保留给高级用户的 Mongo 覆盖项
- 已验证“默认参数路径”在 5000 条真实 PostGIS 样本上与原生 `tippecanoe` 字节级一致
- 已验证“默认参数路径”在 20000 条真实 PostGIS 样本上与原生 `tippecanoe` 字节级一致
- 已验证 20000 条真实样本在 Mongo 侧正常落库，`tileCount=63`、`metaCount=1`

完成标准：

- 关键路径有最小保障
- 改动不会轻易破坏现有主链路

## 实施顺序

严格按下面顺序推进：

1. 先做参数分层定义
2. 再改 `maindb.cpp` 的参数解析和默认值归一化
3. 再收口 PostGIS 输入层默认行为
4. 再收口 Mongo/MBTiles 输出语义
5. 最后改帮助信息和测试

## 风险清单

- [ ] 风险 1：误伤现有兼容参数
  处理：隐藏而不是直接删除

- [ ] 风险 2：帮助信息修改影响解析逻辑
  处理：先改解析，再改展示

- [ ] 风险 3：Mongo 输出语义调整影响现有脚本
  处理：保留兼容行为，并对新默认语义输出明确提示

- [ ] 风险 4：覆盖用户现有 `postgis.cpp` 改动
  处理：动该文件前再次核对差异，只做最小必要修改

## 验收标准

- [x] 普通用户只看主帮助就能知道最少怎么用
- [x] 核心命令行足够短，不需要先理解复杂调优参数
- [x] 高级参数仍可用，但不污染主入口
- [x] Mongo 业务输出与 MBTiles 校验输出语义清楚
- [x] 默认值稳定，不传复杂参数也能跑
- [x] `tippecanoe-db` 编译通过
- [x] 最小关键测试通过

## 跟踪规则

后续实施时按下面规则更新本文件：

- 做完一个阶段，就勾掉对应任务
- 如需变更范围，只在本文件追加“变更说明”，不要静默漂移
- 如果发现计划与代码现实不符，优先更新计划，再改代码

## 变更说明

- 2026-04-26：初始化计划文档，作为后续参数收敛与架构收口改造的跟踪基线
- 2026-04-26：实施中发现 `tippecanoe-db` 当前并未像原生 `tippecanoe` 那样暴露 `-l/--layer` 参数，已从“既定核心参数”调整为“待确认是否补充的候选能力”
- 2026-04-26：已将核心帮助参数与运行默认值逻辑抽入 `db_options.hpp`，`maindb.cpp` 已接入；同时确认当前 `unit` 目标存在与本次改动无关的既有链接问题，后续验证需要补充编译级校验
- 2026-04-26：已把 PostGIS / Mongo 的核心连接入口改为“短连接串优先、旧格式兼容”，并放宽了部分原本过严的数据库认证校验，以匹配“核心参数尽量少”的产品目标
- 2026-04-26：已把帮助分层从“默认隐藏”提升为“默认核心帮助 + `--help-advanced` 分层可见”，避免高级用户缺少发现路径
- 2026-04-26：已补充 `--mongo-no-metadata`，并修正默认值归一化逻辑，避免默认值覆盖用户的显式关闭选择
- 2026-04-26：已新增 `postgis_config::normalize()`，并扩展 `mongo_config::normalize()`；Mongo auto batch 现在基于“是否显式设置过 batch size”判断，而不是依赖当前值是否等于默认值
- 2026-04-26：实施中发现 `read_input()` 曾无条件执行 `validate_mongo_config()`，导致“只输出 MBTiles/目录做本地校验”在逻辑上无法成立；现已改为按统一输出模式决定是否要求 Mongo 配置
- 2026-04-26：实施中发现 `db_output_mode::is_dual_output()` 早期只把 `Mongo + MBTiles` 视为双输出，遗漏了 `Mongo + 目录输出`；现已统一按“业务输出 + 任一本地瓦片输出”判定双输出
- 2026-04-26：实施中发现 `--postgis-shard-mode` 的合法值校验仍散落在 `maindb.cpp`；现已回收到 `postgis_config`，并补齐分片模式 helper 语义测试，避免默认策略再次分叉
- 2026-04-26：已再次核对 `tippecanoe-db` 的 `layer` 现状：当前仍未暴露 `-l/--layer` 入口，本轮保持不新增该参数，继续沿用现有 layername 推导路径，避免在参数收敛阶段额外扩面
- 2026-04-26：真实对照验证发现“默认 canonical 属性排序”会让普通用户只用核心参数时无法通过 MBTiles 一致性校验；现已将默认策略调整为“保留源字段顺序”，`--postgis-canonical-attr-order` 降为显式专家覆盖项
