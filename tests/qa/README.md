# tests/qa — 定制功能质检回归脚本

本目录收录 2026-10 质检（见《分支质检报告.md》）P0/P1/P2 修复的回归脚本。
脚本为验证记录的可重放版本：每次修改 checkpoint / db 相关代码后应重跑对应
脚本，确认与既有基线一致。

## 运行约定

- 脚本默认在 `${QA_WORK:-/tmp/qa-fix}` 下产生中间产物（不入库）。
- 二进制默认取仓库根构建产物（`make tippecanoe` / `make tippecanoe-db`），
  可用 `TIPPECANOE_BIN` / `TIPPECANOE_DB_BIN` 环境变量覆盖。
- `tippecanoe-db` 依赖 vcpkg 的 libpq / mongocxx / bsoncxx；本机直跑用：

  ```
  make tippecanoe-db DB_EXTRA_LIBS='-lpq -l:libmongocxx1.so -l:libbsoncxx1.so -lssl -lcrypto -lresolv -lz'
  ```

## 环境依赖

| 依赖 | 用途 |
|---|---|
| PostgreSQL（本地） | 库 `TippTest`，表 `china`、`gc_test`（z6 28 瓦片基准）；`mixed_srid_test` 由脚本自建自删 |
| MongoDB（本地） | `mongodb://admin:admin@localhost:27017/test?authSource=admin` |
| 工具 | psql、mongosh、sqlite3、python3 |

## 数据准备（数据文件不入库）

```bash
# checkpoint 回归用（40 万点，约 350MB）
python3 tests/qa/gen_bigpoints.py /tmp/qa-test/bigpoints.geojson 400000
# 单瓦片超限场景用（1200 万点，约 1.3GB，运行需 -as 收敛）
python3 tests/qa/gen_bigpoints.py /tmp/qa-fix/hugepoints.geojson 12000000
```

注：生成器与质检时所用数据为同规模同分布的等价替代（随机种子固定），
字节级基准比对（oneshot vs resume）在**同一次生成**的数据上进行，与
历史数据不互换。

## 脚本清单与覆盖条目

| 脚本 | 覆盖条目（质检报告编号） |
|---|---|
| run_p1.sh | checkpoint P1 批：mbtiles 模式 ckpt+resume 等价性（v4 状态格式） |
| run_p2.sh | checkpoint P2 批 12 项：M1 resume metadata 保真 / M5 state CRC / M6 blob 截断 / M7 空 blob / M9 退出码 121 / M10 冲突检查前移 / M12 跳过无用指纹 / M13 读入期 SIGTERM / M16 死代码收敛 |
| run_p2dir.sh | 同上，directory 输出模式（CK-S1 metadata 强制覆盖回归） |
| run_t1_t4.sh | T1 常规回归 + T4 (CK-S2) 首 zoom 中断 resume |
| run_t2_t3.sh | CK-S1 dir 模式 metadata 修复 + CK-S3 resume+prefilter 段越界 |
| run_t4b.sh | CK-S2 SIGTERM 读入阶段（last_completed=-1）+ 注入假 map 行后 resume 清理 |
| run_t4c.sh | CK-S2 变体（staging 轮询窗口） |
| run_t5.sh | CK-S2 真实窗口（kill -9 于 entry 快照后、z0 提交前，-as） |
| run_t6.sh | CK-S2 staging-dir 轮询触发 |
| run_t7.sh | CK-S2 两阶段轮询精确命中 z0 提交窗口 |
| run_p1db.sh | db P1 批：DB-S5 E11000 幂等 / DB-M1 SQL 模式 geom 泄漏 / DB-S8 混合 SRID 报错 / gc_test 回归 |
| run_m15.sh | M15 投影：4490 线性网格 decode（单瓦片+整集）、3857 米制 decode 上游语义、交叉组合 prefilter、tile-join crs 透传与混合网格拒绝（exit 101）。数据自建 |
| run_options_refactor.sh | 选项解析层重构：flag 绑定条目（-a@）、prevent 位标志、maindb 补齐的 --name/-N/-A/--output-projection、name 缺省回退、checkpoint 选项专属不串台。依赖 PostGIS TippTest.china；表一致性由 make unit 的 [options] 用例覆盖 |
| cmp_tiles.py | mbtiles 瓦片网格与字节比对工具（脚本内复用） |

## 判定基线

- checkpoint 系列：resume 产物瓦片与 oneshot 一次性运行**字节级一致**
  （`bytes diff: 0`），metadata 字段（除输出名相关）一致。
- db 系列：china 表 Z0-z8 与 `ctid_test.mbtiles` 基准 1154 瓦片 md5 一致；
  gc_test z6 28 瓦片；E11000 重跑 exit 0。
- M15 后新增的投影回归：EPSG:4490 输入的单瓦片 decode 纬度须为线性网格值
  （如 39.9 而非 Mercator 的 78.2）；tile-join 对带 crs 的输入透传 crs 且
  混合 Mercator/线性网格输入时以 exit 101 拒绝。
