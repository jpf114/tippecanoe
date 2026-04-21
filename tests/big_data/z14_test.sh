#!/bin/bash
set -uo pipefail

TIPPECANOE="./tippecanoe"
OUTPUT_DIR="/tmp/tippecanoe_test_output"
REPORT_FILE="/tmp/tippecanoe_test_output/z14_test_results.log"

mkdir -p "$OUTPUT_DIR"
: > "$REPORT_FILE" 2>/dev/null || true

CA_FILE="tests/big_data/California.geojsonl"
NY_FILE="tests/big_data/NewYork.geojsonl"

log_result() {
    echo "$1"
    echo "$1" >> "$REPORT_FILE"
}

run_test() {
    local test_name="$1"
    local output_file="$2"
    shift 2

    log_result ""
    log_result "=========================================="
    log_result "Test: $test_name"
    log_result "Command: $*"
    log_result "Output: $output_file"

    local start_time=$(date +%s.%N)

    /usr/bin/time -v "$@" 2> "${OUTPUT_DIR}/${test_name}.time.log" || {
        local exit_code=$?
        log_result "FAILED with exit code: $exit_code"
        cat "${OUTPUT_DIR}/${test_name}.time.log" >> "$REPORT_FILE"
        return $exit_code
    }

    local end_time=$(date +%s.%N)
    local elapsed=$(echo "$end_time - $start_time" | bc)
    local peak_rss=$(grep "Maximum resident set size" "${OUTPUT_DIR}/${test_name}.time.log" | awk '{print $NF}')
    local user_time=$(grep "User time (seconds)" "${OUTPUT_DIR}/${test_name}.time.log" | awk '{print $NF}')
    local sys_time=$(grep "System time (seconds)" "${OUTPUT_DIR}/${test_name}.time.log" | awk '{print $NF}')

    local output_size=0
    if [ -f "$output_file" ]; then
        output_size=$(stat -c%s "$output_file" 2>/dev/null || echo 0)
    fi

    local tile_count=0
    if [ -f "$output_file" ] && [[ "$output_file" == *.mbtiles ]]; then
        tile_count=$(sqlite3 "$output_file" "SELECT COUNT(*) FROM tiles;" 2>/dev/null || echo 0)
    fi

    local peak_rss_mb=$(echo "scale=1; $peak_rss / 1024" | bc 2>/dev/null || echo "N/A")
    local output_size_mb=$(echo "scale=1; $output_size / 1048576" | bc 2>/dev/null || echo "N/A")

    log_result "Wall time: ${elapsed}s"
    log_result "User time: ${user_time}s"
    log_result "System time: ${sys_time}s"
    log_result "Peak RSS: ${peak_rss} KB (${peak_rss_mb} MB)"
    log_result "Output size: ${output_size} bytes (${output_size_mb} MB)"
    log_result "Tile count: ${tile_count}"

    if [ -f "$output_file" ]; then
        local min_zoom=$(sqlite3 "$output_file" "SELECT MIN(zoom_level) FROM tiles;" 2>/dev/null || echo "N/A")
        local max_zoom=$(sqlite3 "$output_file" "SELECT MAX(zoom_level) FROM tiles;" 2>/dev/null || echo "N/A")
        local total_tiles=$(sqlite3 "$output_file" "SELECT COUNT(*) FROM tiles;" 2>/dev/null || echo "N/A")
        log_result "Min zoom: $min_zoom, Max zoom: $max_zoom, Total tiles: $total_tiles"

        for z in $(seq $min_zoom $max_zoom 2>/dev/null); do
            local z_count=$(sqlite3 "$output_file" "SELECT COUNT(*) FROM tiles WHERE zoom_level=$z;" 2>/dev/null || echo 0)
            local z_max=$(sqlite3 "$output_file" "SELECT MAX(LENGTH(tile_data)) FROM tiles WHERE zoom_level=$z;" 2>/dev/null || echo 0)
            log_result "  Zoom $z: $z_count tiles, max=${z_max}B"
        done
    fi
}

cleanup() {
    find "$OUTPUT_DIR" -name "*.mbtiles" -exec rm -f {} \; 2>/dev/null || true
}

# ================================================================
# z14 Tests
# ================================================================
log_result "============================================================"
log_result "TEST 1: California z14 - Strong optimization (drop-densest + coalesce + simplification)"
run_test "ca_z14_strong_opt" \
    "${OUTPUT_DIR}/ca_z14_strong_opt.mbtiles" \
    $TIPPECANOE -q -f -z14 -P \
    -r5 \
    --drop-densest-as-needed \
    --coalesce-densest-as-needed \
    --simplification=10 \
    -o "${OUTPUT_DIR}/ca_z14_strong_opt.mbtiles" \
    "$CA_FILE"

cleanup

log_result ""
log_result "============================================================"
log_result "TEST 2: California z14 - More aggressive drop rate"
run_test "ca_z14_aggressive" \
    "${OUTPUT_DIR}/ca_z14_aggressive.mbtiles" \
    $TIPPECANOE -q -f -z14 -P \
    -r10 \
    --drop-densest-as-needed \
    --coalesce-densest-as-needed \
    --simplification=10 \
    -o "${OUTPUT_DIR}/ca_z14_aggressive.mbtiles" \
    "$CA_FILE"

cleanup

log_result ""
log_result "============================================================"
log_result "TEST 3: California z14 - Maximum tile bytes limit + aggressive optimization"
run_test "ca_z14_max_tile_limit" \
    "${OUTPUT_DIR}/ca_z14_max_tile_limit.mbtiles" \
    $TIPPECANOE -q -f -z14 -P \
    -r10 \
    --drop-densest-as-needed \
    --coalesce-densest-as-needed \
    --simplification=10 \
    --maximum-tile-bytes=512000 \
    -o "${OUTPUT_DIR}/ca_z14_max_tile_limit.mbtiles" \
    "$CA_FILE"

cleanup

log_result ""
log_result "============================================================"
log_result "TEST 4: California z14 - Drop fraction as needed (strongest)"
run_test "ca_z14_drop_fraction" \
    "${OUTPUT_DIR}/ca_z14_drop_fraction.mbtiles" \
    $TIPPECANOE -q -f -z14 -P \
    -r10 \
    --drop-densest-as-needed \
    --drop-fraction-as-needed \
    --coalesce-densest-as-needed \
    --simplification=10 \
    -o "${OUTPUT_DIR}/ca_z14_drop_fraction.mbtiles" \
    "$CA_FILE"

cleanup

# ================================================================
# NY z14 Tests
# ================================================================
log_result ""
log_result "============================================================"
log_result "TEST 5: NewYork z14 - Strong optimization"
run_test "ny_z14_strong_opt" \
    "${OUTPUT_DIR}/ny_z14_strong_opt.mbtiles" \
    $TIPPECANOE -q -f -z14 -P \
    -r5 \
    --drop-densest-as-needed \
    --coalesce-densest-as-needed \
    --simplification=10 \
    -o "${OUTPUT_DIR}/ny_z14_strong_opt.mbtiles" \
    "$NY_FILE"

cleanup

# ================================================================
# Merged z14 Tests
# ================================================================
log_result ""
log_result "============================================================"
log_result "TEST 6: Merged z14 - Strong optimization"
run_test "merged_z14_strong_opt" \
    "${OUTPUT_DIR}/merged_z14_strong_opt.mbtiles" \
    $TIPPECANOE -q -f -z14 -P \
    -r10 \
    --drop-densest-as-needed \
    --coalesce-densest-as-needed \
    --simplification=10 \
    --drop-fraction-as-needed \
    -o "${OUTPUT_DIR}/merged_z14_strong_opt.mbtiles" \
    "$CA_FILE" "$NY_FILE"

cleanup

log_result ""
log_result "============================================================"
log_result "ALL Z14+ TESTS COMPLETED"
log_result "============================================================"
