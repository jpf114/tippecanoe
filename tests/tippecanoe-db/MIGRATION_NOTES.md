# tippecanoe-db 测试目录迁移说明

## 迁移日期
2026-03-13

## 迁移内容

已将 tippecanoe-db 参数验证测试的所有相关文件迁移到专用测试目录：

**目标目录**: `tests/tippecanoe-db/`

## 文件清单

以下文件已移动到 `tests/tippecanoe-db/` 目录：

1. **README.md** - 测试套件使用说明
2. **quick_parameter_validation.sh** - 快速验证脚本（10 个测试）
3. **test_tippecanoe_db_parameters.sh** - 完整测试脚本（160+ 测试）
4. **generate_test_data.sql** - 测试数据生成脚本
5. **tippecanoe-db-parameter-validation-report.md** - 详细验证报告
6. **tippecanoe-db-validation-summary.md** - 验证总结

## 目录结构

```
tippecanoe/
├── tests/
│   └── tippecanoe-db/           # tippecanoe-db 专用测试目录
│       ├── README.md
│       ├── quick_parameter_validation.sh
│       ├── test_tippecanoe_db_parameters.sh
│       ├── generate_test_data.sql
│       ├── tippecanoe-db-parameter-validation-report.md
│       └── tippecanoe-db-validation-summary.md
├── tippecanoe-db                # 编译生成的执行体
└── ... (其他项目文件)
```

## 使用方法

### 从项目根目录运行

```bash
cd /home/tdt-dell/code/GitHubCode/tippecanoe

# 运行快速验证
tests/tippecanoe-db/quick_parameter_validation.sh

# 运行完整测试
tests/tippecanoe-db/test_tippecanoe_db_parameters.sh
```

### 从测试目录运行

```bash
cd /home/tdt-dell/code/GitHubCode/tippecanoe/tests/tippecanoe-db

# 运行快速验证
./quick_parameter_validation.sh

# 运行完整测试
./test_tippecanoe_db_parameters.sh
```

## 改进说明

### 路径自动检测

测试脚本已更新为自动检测 `tippecanoe-db` 执行体路径，支持以下位置：
1. 项目根目录 (`../../tippecanoe-db`)
2. 当前目录 (`./tippecanoe-db`)
3. 系统 PATH 中的 `tippecanoe-db`

### 测试组织

所有 tippecanoe-db 相关测试现在都集中在一个目录中，便于：
- 维护和管理
- 运行特定测试
- 查看测试文档
- 添加新测试

## 验证结果

迁移后已验证：
- ✅ 快速验证脚本正常运行（10/10 测试通过）
- ✅ 路径自动检测功能正常
- ✅ 所有文档文件完整
- ✅ 目录结构清晰

## 后续工作

1. 在 `tests/` 主目录的 README 中添加 tippecanoe-db 测试的说明
2. 考虑将测试集成到 CI/CD 流程中
3. 添加性能基准测试

## 参考文档

详细使用说明请参考：
- [tests/tippecanoe-db/README.md](tests/tippecanoe-db/README.md)
- [tests/tippecanoe-db/tippecanoe-db-validation-summary.md](tests/tippecanoe-db/tippecanoe-db-validation-summary.md)

---
**迁移执行日期**: 2026-03-13  
**执行者**: AI Assistant
