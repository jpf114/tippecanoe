#!/bin/bash
# tippecanoe-db 快速参数验证脚本
# 用于验证关键参数的基本功能

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

OUTPUT_DIR="/tmp/tippecanoe_quick_tests"
# 自动检测 tippecanoe-db 路径（支持从 tests 目录或项目根目录运行）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -f "$SCRIPT_DIR/../../tippecanoe-db" ]; then
    TIPPECANOE_DB="$SCRIPT_DIR/../../tippecanoe-db"
elif [ -f "$SCRIPT_DIR/tippecanoe-db" ]; then
    TIPPECANOE_DB="$SCRIPT_DIR/tippecanoe-db"
elif [ -f "./tippecanoe-db" ]; then
    TIPPECANOE_DB="./tippecanoe-db"
else
    TIPPECANOE_DB="$(which tippecanoe-db 2>/dev/null || echo "./tippecanoe-db")"
fi

# PostGIS 连接参数（需要根据实际情况修改）
POSTGIS_HOST="${POSTGIS_HOST:-localhost}"
POSTGIS_PORT="${POSTGIS_PORT:-5432}"
POSTGIS_DB="${POSTGIS_DB:-testdb}"
POSTGIS_USER="${POSTGIS_USER:-postgres}"
POSTGIS_PASSWORD="${POSTGIS_PASSWORD:-postgres}"

echo "========================================"
echo "tippecanoe-db 快速参数验证"
echo "========================================"
echo ""

# 创建输出目录
mkdir -p "$OUTPUT_DIR"

# 测试 1: 版本信息
echo -e "${BLUE}[TEST 1]${NC} 测试版本信息..."
$TIPPECANOE_DB --version
echo -e "${GREEN}✓ 通过${NC}"
echo ""

# 测试 2: 帮助信息
echo -e "${BLUE}[TEST 2]${NC} 测试帮助信息..."
$TIPPECANOE_DB -H > /dev/null 2>&1
echo -e "${GREEN}✓ 通过${NC}"
echo ""

# 测试 3: 参数验证 - 缺少输出
echo -e "${BLUE}[TEST 3]${NC} 测试缺少输出参数（应该失败）..."
if $TIPPECANOE_DB --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" -z2 -q 2>/dev/null; then
    echo -e "${RED}✗ 失败 - 应该报错但没有${NC}"
else
    echo -e "${GREEN}✓ 通过 - 正确报错${NC}"
fi
echo ""

# 测试 4: 参数验证 - 输出互斥
echo -e "${BLUE}[TEST 4]${NC} 测试 -o 和 -e 互斥（应该失败）..."
if $TIPPECANOE_DB --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" -z2 -o "$OUTPUT_DIR/test.mbtiles" -e "$OUTPUT_DIR/test_dir" -q 2>/dev/null; then
    echo -e "${RED}✗ 失败 - 应该报错但没有${NC}"
else
    echo -e "${GREEN}✓ 通过 - 正确报错${NC}"
fi
echo ""

# 测试 5: 参数验证 - 无效的 zoom 级别
echo -e "${BLUE}[TEST 5]${NC} 测试 minzoom > maxzoom（应该失败）..."
if $TIPPECANOE_DB --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" -Z10 -z5 -o "$OUTPUT_DIR/test.mbtiles" -f -q 2>/dev/null; then
    echo -e "${RED}✗ 失败 - 应该报错但没有${NC}"
else
    echo -e "${GREEN}✓ 通过 - 正确报错${NC}"
fi
echo ""

# 测试 6: 参数边界值 - detail
echo -e "${BLUE}[TEST 6]${NC} 测试 detail 边界值 31（应该失败）..."
if $TIPPECANOE_DB --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" -z2 -d31 -o "$OUTPUT_DIR/test.mbtiles" -f -q 2>/dev/null; then
    echo -e "${RED}✗ 失败 - 应该报错但没有${NC}"
else
    echo -e "${GREEN}✓ 通过 - 正确报错${NC}"
fi
echo ""

# 测试 7: 参数边界值 - buffer
echo -e "${BLUE}[TEST 7]${NC} 测试 buffer 边界值 128（应该失败）..."
if $TIPPECANOE_DB --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" -z2 -b128 -o "$OUTPUT_DIR/test.mbtiles" -f -q 2>/dev/null; then
    echo -e "${RED}✗ 失败 - 应该报错但没有${NC}"
else
    echo -e "${GREEN}✓ 通过 - 正确报错${NC}"
fi
echo ""

# 测试 8: 参数边界值 - cluster-distance
echo -e "${BLUE}[TEST 8]${NC} 测试 cluster-distance 边界值 256（应该失败）..."
if $TIPPECANOE_DB --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" -z2 -K256 -o "$OUTPUT_DIR/test.mbtiles" -f -q 2>/dev/null; then
    echo -e "${RED}✗ 失败 - 应该报错但没有${NC}"
else
    echo -e "${GREEN}✓ 通过 - 正确报错${NC}"
fi
echo ""

# 测试 9: 参数边界值 - drop-denser
echo -e "${BLUE}[TEST 9]${NC} 测试 drop-denser 边界值 101（应该失败）..."
if $TIPPECANOE_DB --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" -z2 --drop-denser=101 -o "$OUTPUT_DIR/test.mbtiles" -f -q 2>/dev/null; then
    echo -e "${RED}✗ 失败 - 应该报错但没有${NC}"
else
    echo -e "${GREEN}✓ 通过 - 正确报错${NC}"
fi
echo ""

# 测试 10: 简化参数
echo -e "${BLUE}[TEST 10]${NC} 测试 simplification <= 0（应该失败）..."
if $TIPPECANOE_DB --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" -z2 -S0 -o "$OUTPUT_DIR/test.mbtiles" -f -q 2>/dev/null; then
    echo -e "${RED}✗ 失败 - 应该报错但没有${NC}"
else
    echo -e "${GREEN}✓ 通过 - 正确报错${NC}"
fi
echo ""

echo "========================================"
echo "快速验证完成！"
echo "========================================"
echo ""
echo "注意：以上测试主要验证参数验证逻辑。"
echo "要执行完整的数据库连接测试，需要配置正确的 PostGIS 数据库连接参数。"
echo ""
echo "设置环境变量："
echo "  export POSTGIS_HOST=your_host"
echo "  export POSTGIS_PORT=your_port"
echo "  export POSTGIS_DB=your_database"
echo "  export POSTGIS_USER=your_user"
echo "  export POSTGIS_PASSWORD=your_password"
echo ""

# 清理
rm -rf "$OUTPUT_DIR"
