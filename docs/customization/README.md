# Tippecanoe 定制版文档专区

> 本目录集中收录本仓库相对上游（Felt/tippecanoe `main`）的全部定制内容。
> 仓库根的 [README.md](../../README.md) 保持与上游逐字一致；所有定制说明只在本目录维护。
> 历史过程文档（设计稿、质检报告、早期方案）存于 [history/](history/)，内容以本目录的当前态文档为准。

## 定制功能总览

| 功能 | 入口二进制 | 核心文档 |
|---|---|---|
| PostGIS 直读 + MongoDB 业务输出 + MBTiles/目录瓦片校验输出（三输出模式） | `tippecanoe-db` | [02-参数参考](02-参数参考.md) · [03-架构设计](03-架构设计.md) |
| 断点续做（checkpoint/resume，含读入阶段快照） | `tippecanoe` / `tippecanoe-db` | [04-断点续做](04-断点续做.md) |
| vendor 三方库（离线编译，无 vcpkg 依赖） | 构建期 | [01-构建指南](01-构建指南.md) |
| 质量回归套件（unit + P2 + china 基准） | `make test` / `tests/qa/` | [06-QA回归手册](06-QA回归手册.md) |
| 性能实测与瓶颈归因 | - | [05-性能报告](05-性能报告.md) |

## 分支结构

```
main                       ← 上游 Felt/tippecanoe，保持纯净（无任何定制代码/文档）
felt/main (remote)         ← 上游镜像
feature/checkpoint-resume  ← 定制分支（已合并，冻结保留，勿再修改）
feature/db                 ← 定制分支（已合并，冻结保留，勿再修改）
feature/combined           ← 开发主线：两定制分支合并后的全部定制 + 后续演进
```

- `feature/combined` 包含两个定制分支的全部提交（`git merge-base --is-ancestor` 已验证）。
- 两个旧定制分支自 2026-10-16 22:53 起冻结。未来如需在旧分支上恢复开发，从
  `feature/combined` cherry-pick 本目录的文档提交即可保持文档组织一致。

## 快速上手

```bash
# 1. 构建 vendor 三方库（首次，源码已随仓库携带，离线可执行）
./third_party/build_all.sh

# 2. 构建全部二进制（核心 6 个 + tippecanoe-db）
make all-db

# 3. 运行（PostGIS 输入 + MBTiles 校验输出，最简示例）
./tippecanoe-db -f -Z0 -z8 -o out.mbtiles \
  --postgis postgresql://postgres:postgres@127.0.0.1:5432/TippTest \
  --postgis-table china --postgis-geometry-field geom

# 4. 回归验证
make test
bash tests/qa/run_p2.sh
```

## 文档地图

```
docs/customization/
├── README.md          ← 本文件（总索引）
├── 01-构建指南.md      ← 三方库、编译、离线部署
├── 02-参数参考.md      ← 全部新增选项、默认值、修改方式
├── 03-架构设计.md      ← 技术设计（含图）：管线、收敛、选项机制
├── 04-断点续做.md      ← checkpoint/resume 使用与机制
├── 05-性能报告.md      ← 性能矩阵实测与归因
├── 06-QA回归手册.md   ← 回归套件与基线
└── history/           ← 历史过程文档存档（15 篇，内容以当前态为准）
```

## 与上游的差异边界

- **README.md / man page / CHANGELOG.md**：与上游一致（定制说明全部收拢于本目录）。
- **新增源码**：`tippecanoe-db` 相关（maindb、mongo、postgis、tile-db、checkpoint、
  error_logger、options_table、common_main 等）与 `third_party/` vendor 库。
- **修改的上游源码**：`tile.cpp`（输出 sink 三钩子）、`Makefile`（vendor 优先）、
  选项表与投影等改动详见 [03-架构设计](03-架构设计.md) 第 5 节。
