#!/bin/bash
set -uo pipefail

TIPPECANOE="./tippecanoe"
OUTPUT_DIR="/tmp/tippecanoe_test_output"
REPORT_FILE="/tmp/tippecanoe_test_output/test_results.log"

mkdir -p "$OUTPUT_DIR"
: > "$REPORT_FILE" 2>/dev/null || true

CA_FILE="tests/big_data/California.geojsonl"
NY_FILE="tests/big_data/NewYork.geojsonl"

log_section() {
    echo ""
    echo "================================================================"
    echo "$1"
    echo "================================================================"
    echo "" >> "$REPORT_FILE"
    echo "================================================================" >> "$REPORT_FILE"
    echo "$1" >> "$REPORT_FILE"
    echo "================================================================" >> "$REPORT_FILE"
}

log_result() {
    echo "$1"
    echo "$1" >> "$REPORT_FILE"
}

run_test() {
    local test_name="$1"
    local output_file="$2"
    shift 2
    local cmd="$@"

    log_result ""
    log_result "--- Test: $test_name ---"
    log_result "Command: $cmd"
    log_result "Output: $output_file"

    local start_time=$(date +%s.%N)

    /usr/bin/time -v $cmd 2> "${OUTPUT_DIR}/${test_name}.time.log" || {
        local exit_code=$?
        log_result "FAILED with exit code: $exit_code"
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
    echo ""
}

get_tile_stats() {
    local mbtiles_file="$1"
    if [ -f "$mbtiles_file" ]; then
        local min_zoom=$(sqlite3 "$mbtiles_file" "SELECT MIN(zoom_level) FROM tiles;" 2>/dev/null || echo "N/A")
        local max_zoom=$(sqlite3 "$mbtiles_file" "SELECT MAX(zoom_level) FROM tiles;" 2>/dev/null || echo "N/A")
        local total_tiles=$(sqlite3 "$mbtiles_file" "SELECT COUNT(*) FROM tiles;" 2>/dev/null || echo "N/A")

        log_result "Min zoom: $min_zoom"
        log_result "Max zoom: $max_zoom"
        log_result "Total tiles: $total_tiles"

        for z in $(seq $min_zoom $max_zoom 2>/dev/null); do
            local z_count=$(sqlite3 "$mbtiles_file" "SELECT COUNT(*) FROM tiles WHERE zoom_level=$z;" 2>/dev/null || echo 0)
            local z_max=$(sqlite3 "$mbtiles_file" "SELECT MAX(LENGTH(tile_data)) FROM tiles WHERE zoom_level=$z;" 2>/dev/null || echo 0)
            log_result "  Zoom $z: $z_count tiles, max_size=${z_max}B"
        done
    fi
}

cleanup() {
    find "$OUTPUT_DIR" -name "*.mbtiles" -exec rm -f {} \; 2>/dev/null || true
}

# ================================================================
# PART 1: California Single File Tests (3.6GB, 11.5M features)
# ================================================================
log_section "PART 1: California Single File Tests"

# 1.1: Low zoom (z5) - baseline
run_test "ca_z5_sequential" \
    "${OUTPUT_DIR}/ca_z5_sequential.mbtiles" \
    $TIPPECANOE -q -f -z5 -o "${OUTPUT_DIR}/ca_z5_sequential.mbtiles" "$CA_FILE"
get_tile_stats "${OUTPUT_DIR}/ca_z5_sequential.mbtiles"

# 1.2: Low zoom (z5) - parallel mode
run_test "ca_z5_parallel" \
    "${OUTPUT_DIR}/ca_z5_parallel.mbtiles" \
    $TIPPECANOE -q -f -z5 -P -o "${OUTPUT_DIR}/ca_z5_parallel.mbtiles" "$CA_FILE"

cleanup

# 1.3: Medium zoom (z8) - default
run_test "ca_z8_default" \
    "${OUTPUT_DIR}/ca_z8_default.mbtiles" \
    $TIPPECANOE -q -f -z8 -o "${OUTPUT_DIR}/ca_z8_default.mbtiles" "$CA_FILE"
get_tile_stats "${OUTPUT_DIR}/ca_z8_default.mbtiles"

# 1.4: Medium zoom (z8) - with optimization
run_test "ca_z8_optimized" \
    "${OUTPUT_DIR}/ca_z8_optimized.mbtiles" \
    $TIPPECANOE -q -f -z8 -r5 --drop-densest-as-needed --simplification=10 -o "${OUTPUT_DIR}/ca_z8_optimized.mbtiles" "$CA_FILE"
get_tile_stats "${OUTPUT_DIR}/ca_z8_optimized.mbtiles"

cleanup

# 1.5: High zoom (z12) - default
run_test "ca_z12_default" \
    "${OUTPUT_DIR}/ca_z12_default.mbtiles" \
    $TIPPECANOE -q -f -z12 -o "${OUTPUT_DIR}/ca_z12_default.mbtiles" "$CA_FILE"
get_tile_stats "${OUTPUT_DIR}/ca_z12_default.mbtiles"

# 1.6: High zoom (z12) - optimized + parallel
run_test "ca_z12_parallel_optimized" \
    "${OUTPUT_DIR}/ca_z12_parallel_optimized.mbtiles" \
    $TIPPECANOE -q -f -z12 -P -r5 --drop-densest-as-needed --coalesce-densest-as-needed --simplification=10 -o "${OUTPUT_DIR}/ca_z12_parallel_optimized.mbtiles" "$CA_FILE"
get_tile_stats "${OUTPUT_DIR}/ca_z12_parallel_optimized.mbtiles"

cleanup

# 1.7: Auto zoom detection
run_test "ca_auto_zoom" \
    "${OUTPUT_DIR}/ca_auto_zoom.mbtiles" \
    $TIPPECANOE -q -f -zg -o "${OUTPUT_DIR}/ca_auto_zoom.mbtiles" "$CA_FILE"
get_tile_stats "${OUTPUT_DIR}/ca_auto_zoom.mbtiles"

cleanup

# ================================================================
# PART 2: NewYork Single File Tests (1.4GB, 5M features)
# ================================================================
log_section "PART 2: NewYork Single File Tests"

# 2.1: Low zoom (z5)
run_test "ny_z5_default" \
    "${OUTPUT_DIR}/ny_z5_default.mbtiles" \
    $TIPPECANOE -q -f -z5 -o "${OUTPUT_DIR}/ny_z5_default.mbtiles" "$NY_FILE"
get_tile_stats "${OUTPUT_DIR}/ny_z5_default.mbtiles"

# 2.2: High zoom (z12) - default
run_test "ny_z12_default" \
    "${OUTPUT_DIR}/ny_z12_default.mbtiles" \
    $TIPPECANOE -q -f -z12 -o "${OUTPUT_DIR}/ny_z12_default.mbtiles" "$NY_FILE"
get_tile_stats "${OUTPUT_DIR}/ny_z12_default.mbtiles"

cleanup

# 2.3: High zoom (z12) - parallel + optimized
run_test "ny_z12_parallel_optimized" \
    "${OUTPUT_DIR}/ny_z12_parallel_optimized.mbtiles" \
    $TIPPECANOE -q -f -z12 -P -r5 --drop-densest-as-needed --simplification=10 -o "${OUTPUT_DIR}/ny_z12_parallel_optimized.mbtiles" "$NY_FILE"
get_tile_stats "${OUTPUT_DIR}/ny_z12_parallel_optimized.mbtiles"

# 2.4: Auto zoom detection
run_test "ny_auto_zoom" \
    "${OUTPUT_DIR}/ny_auto_zoom.mbtiles" \
    $TIPPECANOE -q -f -zg -o "${OUTPUT_DIR}/ny_auto_zoom.mbtiles" "$NY_FILE"
get_tile_stats "${OUTPUT_DIR}/ny_auto_zoom.mbtiles"

cleanup

# ================================================================
# PART 3: Merged File Tests (CA + NY, ~5GB, ~16.5M features)
# ================================================================
log_section "PART 3: Merged File Tests (California + NewYork)"

# 3.1: Low zoom (z5) - sequential
run_test "merged_z5_sequential" \
    "${OUTPUT_DIR}/merged_z5_sequential.mbtiles" \
    $TIPPECANOE -q -f -z5 -o "${OUTPUT_DIR}/merged_z5_sequential.mbtiles" "$CA_FILE" "$NY_FILE"
get_tile_stats "${OUTPUT_DIR}/merged_z5_sequential.mbtiles"

# 3.2: Low zoom (z5) - parallel
run_test "merged_z5_parallel" \
    "${OUTPUT_DIR}/merged_z5_parallel.mbtiles" \
    $TIPPECANOE -q -f -z5 -P -o "${OUTPUT_DIR}/merged_z5_parallel.mbtiles" "$CA_FILE" "$NY_FILE"
get_tile_stats "${OUTPUT_DIR}/merged_z5_parallel.mbtiles"

cleanup

# 3.3: High zoom (z12) - sequential
run_test "merged_z12_sequential" \
    "${OUTPUT_DIR}/merged_z12_sequential.mbtiles" \
    $TIPPECANOE -q -f -z12 -o "${OUTPUT_DIR}/merged_z12_sequential.mbtiles" "$CA_FILE" "$NY_FILE"
get_tile_stats "${OUTPUT_DIR}/merged_z12_sequential.mbtiles"

# 3.4: High zoom (z12) - parallel + optimized
run_test "merged_z12_parallel_optimized" \
    "${OUTPUT_DIR}/merged_z12_parallel_optimized.mbtiles" \
    $TIPPECANOE -q -f -z12 -P -r5 --drop-densest-as-needed --coalesce-densest-as-needed --simplification=10 -o "${OUTPUT_DIR}/merged_z12_parallel_optimized.mbtiles" "$CA_FILE" "$NY_FILE"
get_tile_stats "${OUTPUT_DIR}/merged_z12_parallel_optimized.mbtiles"

cleanup

# 3.5: Merged with separate named layers using -L
run_test "merged_z8_separate_layers" \
    "${OUTPUT_DIR}/merged_z8_separate_layers.mbtiles" \
    $TIPPECANOE -q -f -z8 -P \
        -L'{"file":"tests/big_data/California.geojsonl","layer":"California"}' \
        -L'{"file":"tests/big_data/NewYork.geojsonl","layer":"NewYork"}' \
        -o "${OUTPUT_DIR}/merged_z8_separate_layers.mbtiles"
get_tile_stats "${OUTPUT_DIR}/merged_z8_separate_layers.mbtiles"

cleanup

log_section "ALL TESTS COMPLETED"
echo "Full results saved to: $REPORT_FILE"
