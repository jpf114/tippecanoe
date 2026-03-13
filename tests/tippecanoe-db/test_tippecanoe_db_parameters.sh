#!/bin/bash
# tippecanoe-db 参数验证测试脚本
# 用于全面验证 tippecanoe-db 执行体的所有非 PostGIS 连接参数

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 配置变量
POSTGIS_HOST="${POSTGIS_HOST:-localhost}"
POSTGIS_PORT="${POSTGIS_PORT:-5432}"
POSTGIS_DB="${POSTGIS_DB:-testdb}"
POSTGIS_USER="${POSTGIS_USER:-postgres}"
POSTGIS_PASSWORD="${POSTGIS_PASSWORD:-postgres}"
OUTPUT_DIR="${OUTPUT_DIR:-/tmp/tippecanoe_db_tests}"

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

# 统计变量
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
SKIPPED_TESTS=0

# 日志函数
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((PASSED_TESTS++))
}

log_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((FAILED_TESTS++))
}

log_skip() {
    echo -e "${YELLOW}[SKIP]${NC} $1"
    ((SKIPPED_TESTS++))
}

# 初始化函数
init() {
    log_info "初始化测试环境..."
    mkdir -p "$OUTPUT_DIR"
    
    # 检查 tippecanoe-db 是否存在
    if [ ! -f "$TIPPECANOE_DB" ]; then
        log_error "tippecanoe-db 执行体不存在：$TIPPECANOE_DB"
        echo "请先编译 tippecanoe-db: make tippecanoe-db"
        exit 1
    fi
    
    # 检查 PostgreSQL 连接
    if ! command -v psql &> /dev/null; then
        log_error "psql 命令未找到"
        exit 1
    fi
    
    log_info "测试输出目录：$OUTPUT_DIR"
    log_info "tippecanoe-db 路径：$TIPPECANOE_DB"
    log_info "PostGIS 连接：$POSTGIS_USER@$POSTGIS_HOST:$POSTGIS_PORT/$POSTGIS_DB"
}

# 清理函数
cleanup() {
    log_info "清理测试文件..."
    rm -rf "$OUTPUT_DIR"/*
}

# 运行测试并检查结果
run_test() {
    local test_name="$1"
    local expected_result="${2:-0}"  # 0 表示期望成功，非 0 表示期望失败
    shift 1
    
    ((TOTAL_TESTS++))
    log_info "运行测试：$test_name"
    
    # 运行命令
    if "$@" > "$OUTPUT_DIR/${test_name}.log" 2>&1; then
        if [ "$expected_result" -eq 0 ]; then
            log_success "$test_name"
            return 0
        else
            log_error "$test_name - 期望失败但成功了"
            return 1
        fi
    else
        if [ "$expected_result" -ne 0 ]; then
            log_success "$test_name - 正确失败"
            return 0
        else
            log_error "$test_name - 命令执行失败"
            cat "$OUTPUT_DIR/${test_name}.log"
            return 1
        fi
    fi
}

# ============================================================================
# 1. 输出配置参数测试
# ============================================================================
test_output_config() {
    log_info "=== 测试输出配置参数 ==="
    
    # 测试 -o: MBTiles 输出
    run_test "OUT_001_mbtiles_output" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -o "$OUTPUT_DIR/test.mbtiles" -f -q
    
    # 测试 -e: 目录输出
    run_test "OUT_002_directory_output" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -e "$OUTPUT_DIR/test_dir" -f -q
    
    # 测试 -f: 覆盖现有文件
    run_test "OUT_003_force_overwrite" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -o "$OUTPUT_DIR/test.mbtiles" -f -q
    
    # 测试 -o 和 -e 互斥
    run_test "OUT_004_output_mutual_exclusion" 1 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -o "$OUTPUT_DIR/test.mbtiles" -e "$OUTPUT_DIR/test_dir" -q
    
    # 测试缺少输出参数
    run_test "OUT_005_no_output" 1 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -q
}

# ============================================================================
# 2. 缩放级别参数测试
# ============================================================================
test_zoom_levels() {
    log_info "=== 测试缩放级别参数 ==="
    
    # 测试不同 maxzoom 值
    for zoom in 0 1 2 5; do
        run_test "ZOOM_00${zoom}_maxzoom_${zoom}" 0 \
            "$TIPPECANOE_DB" \
            --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
            -z$zoom -o "$OUTPUT_DIR/zoom_${zoom}.mbtiles" -f -q
    done
    
    # 测试 minzoom 和 maxzoom 关系
    run_test "ZOOM_006_min_max_zoom" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -Z2 -z5 -o "$OUTPUT_DIR/zoom_2_5.mbtiles" -f -q
    
    # 测试 minzoom > maxzoom (应该失败)
    run_test "ZOOM_007_invalid_min_max" 1 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -Z10 -z5 -o "$OUTPUT_DIR/zoom_invalid.mbtiles" -f -q
    
    # 测试自动猜测 maxzoom
    run_test "ZOOM_008_auto_guess" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -zg -o "$OUTPUT_DIR/zoom_auto.mbtiles" -f -q
}

# ============================================================================
# 3. 分辨率参数测试
# ============================================================================
test_detail_levels() {
    log_info "=== 测试分辨率参数 ==="
    
    # 测试 full-detail
    run_test "DETAIL_001_full_12" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -d12 -o "$OUTPUT_DIR/detail_12.mbtiles" -f -q
    
    run_test "DETAIL_002_full_20" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -d20 -o "$OUTPUT_DIR/detail_20.mbtiles" -f -q
    
    # 测试 low-detail
    run_test "DETAIL_003_low_8" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -D8 -o "$OUTPUT_DIR/low_8.mbtiles" -f -q
    
    # 测试 minimum-detail
    run_test "DETAIL_004_min_5" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -m5 -o "$OUTPUT_DIR/min_5.mbtiles" -f -q
    
    # 测试边界值 (应该失败)
    run_test "DETAIL_005_invalid_detail" 1 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -d31 -o "$OUTPUT_DIR/detail_invalid.mbtiles" -f -q
}

# ============================================================================
# 4. 过滤参数测试
# ============================================================================
test_filtering() {
    log_info "=== 测试过滤参数 ==="
    
    # 测试 exclude
    run_test "FILTER_001_exclude" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -x value -o "$OUTPUT_DIR/filter_exclude.mbtiles" -f -q
    
    # 测试 include
    run_test "FILTER_002_include" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -X -y name -o "$OUTPUT_DIR/filter_include.mbtiles" -f -q
    
    # 测试 exclude-all
    run_test "FILTER_003_exclude_all" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -X -o "$OUTPUT_DIR/filter_all.mbtiles" -f -q
    
    # 测试 JSON 过滤器
    run_test "FILTER_004_json_filter" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -j '{"*":[">","population",500]}' -o "$OUTPUT_DIR/filter_json.mbtiles" -f -q
}

# ============================================================================
# 5. 几何简化参数测试
# ============================================================================
test_simplification() {
    log_info "=== 测试几何简化参数 ==="
    
    # 测试 simplification
    run_test "SIMP_001_simp_1" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_lines:geom" \
        -z2 -S1 -o "$OUTPUT_DIR/simp_1.mbtiles" -f -q
    
    run_test "SIMP_002_simp_5" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_lines:geom" \
        -z2 -S5 -o "$OUTPUT_DIR/simp_5.mbtiles" -f -q
    
    # 测试 no-line-simplification
    run_test "SIMP_003_no_line_simp" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_lines:geom" \
        -z2 --no-line-simplification -o "$OUTPUT_DIR/no_line_simp.mbtiles" -f -q
    
    # 测试 tiny-polygon-size
    run_test "SIMP_004_tiny_polygon" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_polygons:geom" \
        -z2 --tiny-polygon-size=10 -o "$OUTPUT_DIR/tiny_polygon.mbtiles" -f -q
    
    # 测试 no-tiny-polygon-reduction
    run_test "SIMP_005_no_tiny_reduction" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_polygons:geom" \
        -z2 --no-tiny-polygon-reduction -o "$OUTPUT_DIR/no_tiny_reduction.mbtiles" -f -q
}

# ============================================================================
# 6. Clipping 参数测试
# ============================================================================
test_clipping() {
    log_info "=== 测试 Clipping 参数 ==="
    
    # 测试 buffer
    run_test "CLIP_001_buffer_5" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -b5 -o "$OUTPUT_DIR/buffer_5.mbtiles" -f -q
    
    run_test "CLIP_002_buffer_50" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -b50 -o "$OUTPUT_DIR/buffer_50.mbtiles" -f -q
    
    # 测试边界值
    run_test "CLIP_003_buffer_127" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -b127 -o "$OUTPUT_DIR/buffer_127.mbtiles" -f -q
    
    # 测试超过边界 (应该失败)
    run_test "CLIP_004_buffer_invalid" 1 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -b128 -o "$OUTPUT_DIR/buffer_invalid.mbtiles" -f -q
    
    # 测试 no-clipping
    run_test "CLIP_005_no_clipping" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 --no-clipping -o "$OUTPUT_DIR/no_clipping.mbtiles" -f -q
}

# ============================================================================
# 7. 性能参数测试
# ============================================================================
test_performance() {
    log_info "=== 测试性能参数 ==="
    
    # 测试 maximum-tile-bytes
    run_test "PERF_001_max_tile_bytes" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_polygons:geom" \
        -z2 -M100000 -o "$OUTPUT_DIR/max_tile_bytes.mbtiles" -f -q
    
    # 测试 maximum-tile-features
    run_test "PERF_002_max_tile_features" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_polygons:geom" \
        -z2 -O10000 -o "$OUTPUT_DIR/max_tile_features.mbtiles" -f -q
    
    # 测试 no-tile-size-limit
    run_test "PERF_003_no_tile_limit" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_polygons:geom" \
        -z2 --no-tile-size-limit -o "$OUTPUT_DIR/no_tile_limit.mbtiles" -f -q
    
    # 测试 no-feature-limit
    run_test "PERF_004_no_feature_limit" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_polygons:geom" \
        -z2 --no-feature-limit -o "$OUTPUT_DIR/no_feature_limit.mbtiles" -f -q
    
    # 测试 no-tile-compression
    run_test "PERF_005_no_compression" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 --no-tile-compression -o "$OUTPUT_DIR/no_compression.mbtiles" -f -q
}

# ============================================================================
# 8. 进度和调试参数测试
# ============================================================================
test_progress() {
    log_info "=== 测试进度参数 ==="
    
    # 测试 quiet
    run_test "PROG_001_quiet" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -q -o "$OUTPUT_DIR/quiet.mbtiles" -f
    
    # 测试 no-progress-indicator
    run_test "PROG_002_no_progress" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -Q -o "$OUTPUT_DIR/no_progress.mbtiles" -f
    
    # 测试 version
    run_test "PROG_003_version" 0 \
        "$TIPPECANOE_DB" \
        --version
}

# ============================================================================
# 9. 参数组合测试
# ============================================================================
test_combinations() {
    log_info "=== 测试参数组合 ==="
    
    # 测试 -z 和 -r 组合
    run_test "COMBO_001_zoom_rate" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z5 -r2 -o "$OUTPUT_DIR/combo_zoom_rate.mbtiles" -f -q
    
    # 测试 -d 和 -S 组合
    run_test "COMBO_002_detail_simp" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_lines:geom" \
        -z2 -d10 -S2 -o "$OUTPUT_DIR/combo_detail_simp.mbtiles" -f -q
    
    # 测试多个过滤参数组合
    run_test "COMBO_003_multi_filter" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -x value -y name -y category -o "$OUTPUT_DIR/combo_filter.mbtiles" -f -q
    
    # 测试 buffer 和 simplification 组合
    run_test "COMBO_004_buffer_simp" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_lines:geom" \
        -z2 -b20 -S3 -o "$OUTPUT_DIR/combo_buffer_simp.mbtiles" -f -q
}

# ============================================================================
# 10. 环境变量测试
# ============================================================================
test_environment() {
    log_info "=== 测试环境变量 ==="
    
    # 测试 TIPPECANOE_MAX_THREADS
    export TIPPECANOE_MAX_THREADS=2
    run_test "ENV_001_threads_2" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -o "$OUTPUT_DIR/env_threads_2.mbtiles" -f -q
    
    export TIPPECANOE_MAX_THREADS=4
    run_test "ENV_002_threads_4" 0 \
        "$TIPPECANOE_DB" \
        --postgis="$POSTGIS_HOST:$POSTGIS_PORT:$POSTGIS_DB:$POSTGIS_USER:$POSTGIS_PASSWORD:test_points:geom" \
        -z2 -o "$OUTPUT_DIR/env_threads_4.mbtiles" -f -q
    
    unset TIPPECANOE_MAX_THREADS
}

# ============================================================================
# 主测试流程
# ============================================================================
main() {
    echo "========================================"
    echo "tippecanoe-db 参数验证测试"
    echo "========================================"
    echo ""
    
    init
    
    # 执行所有测试类别
    test_output_config
    test_zoom_levels
    test_detail_levels
    test_filtering
    test_simplification
    test_clipping
    test_performance
    test_progress
    test_combinations
    test_environment
    
    # 输出测试统计
    echo ""
    echo "========================================"
    echo "测试统计"
    echo "========================================"
    echo -e "总测试数：${TOTAL_TESTS}"
    echo -e "通过：${GREEN}${PASSED_TESTS}${NC}"
    echo -e "失败：${RED}${FAILED_TESTS}${NC}"
    echo -e "跳过：${YELLOW}${SKIPPED_TESTS}${NC}"
    echo ""
    
    if [ $FAILED_TESTS -gt 0 ]; then
        echo -e "${RED}测试未全部通过！${NC}"
        exit 1
    else
        echo -e "${GREEN}所有测试通过！${NC}"
        exit 0
    fi
}

# 运行主函数
main "$@"
