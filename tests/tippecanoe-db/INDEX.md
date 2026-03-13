# tippecanoe-db 测试目录文件索引

## 目录位置
`tests/tippecanoe-db/`

## 文件清单

### 1. 测试脚本 (2 个)

#### quick_parameter_validation.sh
- **大小**: 5.5K
- **用途**: 快速验证参数验证逻辑
- **测试数量**: 10 个
- **执行时间**: ~5 秒
- **数据库要求**: 不需要
- **使用方法**:
  ```bash
  cd tests/tippecanoe-db
  ./quick_parameter_validation.sh
  ```

#### test_tippecanoe_db_parameters.sh
- **大小**: 18K
- **用途**: 全面验证所有参数功能
- **测试数量**: 160+
- **执行时间**: 10-30 分钟
- **数据库要求**: 需要 PostGIS
- **使用方法**:
  ```bash
  cd tests/tippecanoe-db
  export POSTGIS_HOST=localhost
  export POSTGIS_DB=testdb
  export POSTGIS_USER=postgres
  export POSTGIS_PASSWORD=xxx
  ./test_tippecanoe_db_parameters.sh
  ```

### 2. 数据脚本 (1 个)

#### generate_test_data.sql
- **大小**: 6.0K
- **用途**: 生成 PostGIS 测试数据
- **创建表**: 4 个 (test_points, test_lines, test_polygons, test_mixed)
- **要素数量**: 11,750 个
- **使用方法**:
  ```bash
  psql -d testdb -f generate_test_data.sql
  ```

### 3. 文档文件 (4 个)

#### README.md
- **大小**: 7.1K
- **用途**: 测试套件使用说明
- **内容**:
  - 目录结构
  - 快速开始指南
  - 测试脚本说明
  - 参数验证覆盖
  - 常见问题
  - 维护说明

#### tippecanoe-db-parameter-validation-report.md
- **大小**: 19K
- **用途**: 详细验证报告
- **内容**:
  - 56 个参数的详细文档
  - 参数类型、默认值、范围
  - 160+ 测试用例设计
  - 测试结果记录
  - 异常情况和建议

#### tippecanoe-db-validation-summary.md
- **大小**: 6.6K
- **用途**: 验证总结
- **内容**:
  - 执行摘要
  - 验证成果
  - 测试结果
  - 主要发现
  - 建议措施

#### MIGRATION_NOTES.md
- **大小**: 2.8K
- **用途**: 迁移说明
- **内容**:
  - 迁移日期
  - 文件清单
  - 目录结构
  - 使用方法
  - 改进说明

## 快速导航

### 新手入门
1. 阅读 [README.md](README.md)
2. 运行 `./quick_parameter_validation.sh`
3. 查看详细报告 [tippecanoe-db-parameter-validation-report.md](tippecanoe-db-parameter-validation-report.md)

### 完整测试
1. 准备 PostGIS 数据库
2. 运行 `psql -d testdb -f generate_test_data.sql`
3. 运行 `./test_tippecanoe_db_parameters.sh`
4. 查看总结 [tippecanoe-db-validation-summary.md](tippecanoe-db-validation-summary.md)

### 了解迁移
阅读 [MIGRATION_NOTES.md](MIGRATION_NOTES.md)

## 文件统计

| 类型 | 数量 | 总大小 |
|------|------|--------|
| Shell 脚本 | 2 | 23.5K |
| SQL 脚本 | 1 | 6.0K |
| Markdown 文档 | 4 | 35.5K |
| **总计** | **7** | **65K** |

## 测试覆盖

### 参数类别 (12 类)
- ✅ 输出配置 (5 个参数)
- ✅ 缩放级别 (4 个参数)
- ✅ 分辨率 (3 个参数)
- ✅ 过滤 (5 个参数)
- ✅ 属性修改 (6 个参数)
- ✅ 丢弃和聚类 (6 个参数)
- ✅ 几何简化 (4 个参数)
- ✅ Clipping (4 个参数)
- ✅ 排序 (7 个参数)
- ✅ 性能 (6 个参数)
- ✅ 临时存储 (1 个参数)
- ✅ 进度指示 (5 个参数)

### 特殊标志 (16 个)
- ✅ Prevent 标志 (-p)
- ✅ Additional 标志 (-a)

### 环境变量 (1 个)
- ✅ TIPPECANOE_MAX_THREADS

**总计**: 56 个命令行参数 + 16 个标志 + 1 个环境变量 = **73 个配置项**

## 测试场景

### 正常场景
- 参数在有效范围内
- 参数组合正确
- 默认值行为

### 边界场景
- 参数在边界值
- 最大/最小值
- 临界条件

### 错误场景
- 参数超出范围
- 参数类型错误
- 参数互斥冲突

## 版本信息

- **tippecanoe 版本**: v2.80.0
- **构建类型**: Debug
- **测试创建日期**: 2026-03-13
- **最后更新**: 2026-03-13

## 相关资源

- 项目根目录: `../../`
- 源代码: `../../maindb.cpp`
- PostGIS 配置: `../../postgis.hpp`
- 项目文档: `../../README.md`

---
**索引更新日期**: 2026-03-13
