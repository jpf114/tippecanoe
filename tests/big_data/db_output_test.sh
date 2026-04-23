#!/bin/bash
set -euo pipefail

TIPPECANOE_DB="./tippecanoe-db"
OUTPUT_DIR="/tmp/tippecanoe_db_test_output"
LOG_FILE="$OUTPUT_DIR/test_results.log"
TIME_CMD="/usr/bin/time -v"

POSTGIS_CONN="10.1.0.16:5433:geoc_data:postgres:postgres"
MONGO_CONN="localhost:27017:tippecanoe_test:tippecanoe:tippecanoe123:tippecanoe_test"

GEOJSON_FILE="tests/big_data/china.geojson"

mkdir -p "$OUTPUT_DIR"
> "$LOG_FILE"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

run_test() {
    local test_name="$1"
    shift
    local cmd="$@"

    log "=========================================="
    log "TEST: $test_name"
    log "CMD: $cmd"

    local start_time=$(date +%s.%N)

    set +e
    $TIME_CMD -o "$OUTPUT_DIR/${test_name}_time.log" $cmd > "$OUTPUT_DIR/${test_name}_stdout.log" 2> "$OUTPUT_DIR/${test_name}_stderr.log"
    local exit_code=$?
    set -e

    local end_time=$(date +%s.%N)
    local elapsed=$(echo "$end_time - $start_time" | bc)

    local wall_time=$(grep "Wall clock" "$OUTPUT_DIR/${test_name}_time.log" 2>/dev/null | awk '{print $NF}' || echo "N/A")
    local max_rss=$(grep "Maximum resident" "$OUTPUT_DIR/${test_name}_time.log" 2>/dev/null | awk '{print $NF}' || echo "N/A")
    local user_time=$(grep "User time" "$OUTPUT_DIR/${test_name}_time.log" 2>/dev/null | awk '{print $NF}' || echo "N/A")
    local sys_time=$(grep "System time" "$OUTPUT_DIR/${test_name}_time.log" 2>/dev/null | awk '{print $NF}' || echo "N/A")

    log "EXIT_CODE: $exit_code"
    log "WALL_TIME: ${wall_time}s"
    log "USER_TIME: ${user_time}s"
    log "SYS_TIME: ${sys_time}s"
    log "MAX_RSS: ${max_rss} KB"
    log "ELAPSED: ${elapsed}s"

    if [ $exit_code -eq 0 ]; then
        log "STATUS: PASSED"
    else
        log "STATUS: FAILED (exit code: $exit_code)"
        local stderr_tail=$(tail -5 "$OUTPUT_DIR/${test_name}_stderr.log" 2>/dev/null)
        log "STDERR_TAIL: $stderr_tail"
    fi

    log ""
}

clean_mongo() {
    local collection="$1"
    mongosh --host localhost --port 27017 --quiet --eval "db.getSiblingDB('tippecanoe_test').${collection}.drop(); db.getSiblingDB('tippecanoe_test').${collection}_metadata.drop();" 2>/dev/null || true
}

get_mongo_stats() {
    local collection="$1"
    mongosh --host localhost --port 27017 --quiet --eval "
        var db = db.getSiblingDB('tippecanoe_test');
        var count = db.${collection}.countDocuments({});
        var meta = db.${collection}_metadata.countDocuments({});
        var stats = db.${collection}.stats(1024*1024);
        print('DOC_COUNT: ' + count);
        print('META_COUNT: ' + meta);
        print('STORAGE_SIZE_MB: ' + (stats.size / 1024 / 1024).toFixed(2));
        print('DATA_SIZE_MB: ' + (stats.storageSize / 1024 / 1024).toFixed(2));
        print('INDEX_SIZE_MB: ' + (stats.totalIndexSize / 1024 / 1024).toFixed(2));
    " 2>/dev/null
}

get_mbtiles_stats() {
    local mbtiles_file="$1"
    if [ -f "$mbtiles_file" ]; then
        local tile_count=$(sqlite3 "$mbtiles_file" "SELECT count(*) FROM tiles;" 2>/dev/null || echo "N/A")
        local db_size=$(du -h "$mbtiles_file" | awk '{print $1}')
        local zmin=$(sqlite3 "$mbtiles_file" "SELECT min(zoom_level) FROM tiles;" 2>/dev/null || echo "N/A")
        local zmax=$(sqlite3 "$mbtiles_file" "SELECT max(zoom_level) FROM tiles;" 2>/dev/null || echo "N/A")
        echo "TILE_COUNT: $tile_count | SIZE: $db_size | ZMIN: $zmin | ZMAX: $zmax"
    fi
}

log "============================================================"
log "tippecanoe-db PostGIS/MongoDB 输出综合测试"
log "测试时间: $(date)"
log "测试机器: $(hostname)"
log "CPU 核心: $(nproc)"
log "内存: $(free -h | grep Mem | awk '{print $2}')"
log "PostGIS: ${POSTGIS_CONN}:<table>:geom"
log "MongoDB: ${MONGO_CONN}:<collection>"
log "============================================================"
log ""

log "=== PART 1: PostGIS 输入 + MongoDB 输出 (不同数据量/层级) ==="
log ""

log "--- 1.1 小数据: LIGHTS_point (2036条, Point, SRID 900914) ---"
for zoom in 5 8 12; do
    clean_mongo "pg_lights_z${zoom}"
    run_test "pg_mongo_lights_z${zoom}" \
        $TIPPECANOE_DB -q -f -z${zoom} \
        --postgis "${POSTGIS_CONN}:sys_ht_1_10_LIGHTS_point:geom" \
        --mongo "${MONGO_CONN}:pg_lights_z${zoom}"
    log "MONGODB_STATS:"
    get_mongo_stats "pg_lights_z${zoom}" | tee -a "$LOG_FILE"
    log ""
done

log "--- 1.2 中数据: LNDARE_polygon (6256条, Polygon, SRID 900914) ---"
for zoom in 5 8 12; do
    clean_mongo "pg_lndare_z${zoom}"
    run_test "pg_mongo_lndare_z${zoom}" \
        $TIPPECANOE_DB -q -f -z${zoom} \
        --postgis "${POSTGIS_CONN}:sys_ht_1_10_LNDARE_polygon:geom" \
        --mongo "${MONGO_CONN}:pg_lndare_z${zoom}"
    log "MONGODB_STATS:"
    get_mongo_stats "pg_lndare_z${zoom}" | tee -a "$LOG_FILE"
    log ""
done

log "--- 1.3 大数据: SOUNDG_point (31925条, Point, SRID 900914) ---"
for zoom in 5 8 12; do
    clean_mongo "pg_soundg_z${zoom}"
    run_test "pg_mongo_soundg_z${zoom}" \
        $TIPPECANOE_DB -q -f -z${zoom} \
        --postgis "${POSTGIS_CONN}:sys_ht_1_10_SOUNDG_point:geom" \
        --mongo "${MONGO_CONN}:pg_soundg_z${zoom}"
    log "MONGODB_STATS:"
    get_mongo_stats "pg_soundg_z${zoom}" | tee -a "$LOG_FILE"
    log ""
done

log "--- 1.4 大数据: DEPARE_polygon (8431条, Polygon, SRID 900914) ---"
for zoom in 5 8 12; do
    clean_mongo "pg_depare_z${zoom}"
    run_test "pg_mongo_depare_z${zoom}" \
        $TIPPECANOE_DB -q -f -z${zoom} \
        --postgis "${POSTGIS_CONN}:sys_ht_1_10_DEPARE_polygon:geom" \
        --mongo "${MONGO_CONN}:pg_depare_z${zoom}"
    log "MONGODB_STATS:"
    get_mongo_stats "pg_depare_z${zoom}" | tee -a "$LOG_FILE"
    log ""
done

log ""
log "=== PART 2: PostGIS 输入 + MongoDB + MBTiles 双输出 ==="
log ""

log "--- 2.1 LIGHTS_point z8 -> MongoDB + MBTiles ---"
clean_mongo "dual_lights_z8"
rm -f "$OUTPUT_DIR/dual_lights_z8.mbtiles" 2>/dev/null || true
run_test "pg_dual_lights_z8" \
    $TIPPECANOE_DB -q -f -z8 \
    --postgis "${POSTGIS_CONN}:sys_ht_1_10_LIGHTS_point:geom" \
    --mongo "${MONGO_CONN}:dual_lights_z8" \
    -o "$OUTPUT_DIR/dual_lights_z8.mbtiles"
log "MONGODB_STATS:"
get_mongo_stats "dual_lights_z8" | tee -a "$LOG_FILE"
log "MBTILES_STATS: $(get_mbtiles_stats "$OUTPUT_DIR/dual_lights_z8.mbtiles")" | tee -a "$LOG_FILE"
log ""

log "--- 2.2 DEPARE_polygon z8 -> MongoDB + MBTiles ---"
clean_mongo "dual_depare_z8"
rm -f "$OUTPUT_DIR/dual_depare_z8.mbtiles" 2>/dev/null || true
run_test "pg_dual_depare_z8" \
    $TIPPECANOE_DB -q -f -z8 \
    --postgis "${POSTGIS_CONN}:sys_ht_1_10_DEPARE_polygon:geom" \
    --mongo "${MONGO_CONN}:dual_depare_z8" \
    -o "$OUTPUT_DIR/dual_depare_z8.mbtiles"
log "MONGODB_STATS:"
get_mongo_stats "dual_depare_z8" | tee -a "$LOG_FILE"
log "MBTILES_STATS: $(get_mbtiles_stats "$OUTPUT_DIR/dual_depare_z8.mbtiles")" | tee -a "$LOG_FILE"
log ""

log "--- 2.3 SOUNDG_point z8 -> MongoDB + MBTiles ---"
clean_mongo "dual_soundg_z8"
rm -f "$OUTPUT_DIR/dual_soundg_z8.mbtiles" 2>/dev/null || true
run_test "pg_dual_soundg_z8" \
    $TIPPECANOE_DB -q -f -z8 \
    --postgis "${POSTGIS_CONN}:sys_ht_1_10_SOUNDG_point:geom" \
    --mongo "${MONGO_CONN}:dual_soundg_z8" \
    -o "$OUTPUT_DIR/dual_soundg_z8.mbtiles"
log "MONGODB_STATS:"
get_mongo_stats "dual_soundg_z8" | tee -a "$LOG_FILE"
log "MBTILES_STATS: $(get_mbtiles_stats "$OUTPUT_DIR/dual_soundg_z8.mbtiles")" | tee -a "$LOG_FILE"
log ""

log ""
log "=== PART 3: 优化参数测试 (大数据高层级 z14) ==="
log ""

log "--- 3.1 SOUNDG_point z14 优化参数 -> MongoDB ---"
clean_mongo "opt_soundg_z14"
run_test "pg_mongo_soundg_z14_opt" \
    $TIPPECANOE_DB -q -f -z14 \
    -P -r5 --drop-densest-as-needed --coalesce-densest-as-needed --simplification=10 \
    --postgis "${POSTGIS_CONN}:sys_ht_1_10_SOUNDG_point:geom" \
    --mongo "${MONGO_CONN}:opt_soundg_z14"
log "MONGODB_STATS:"
get_mongo_stats "opt_soundg_z14" | tee -a "$LOG_FILE"
log ""

log "--- 3.2 DEPARE_polygon z14 优化参数 -> MongoDB ---"
clean_mongo "opt_depare_z14"
run_test "pg_mongo_depare_z14_opt" \
    $TIPPECANOE_DB -q -f -z14 \
    -P -r5 --drop-densest-as-needed --coalesce-densest-as-needed --simplification=10 \
    --postgis "${POSTGIS_CONN}:sys_ht_1_10_DEPARE_polygon:geom" \
    --mongo "${MONGO_CONN}:opt_depare_z14"
log "MONGODB_STATS:"
get_mongo_stats "opt_depare_z14" | tee -a "$LOG_FILE"
log ""

log "--- 3.3 SOUNDG_point z14 优化参数 -> MongoDB + MBTiles (对比) ---"
clean_mongo "opt_soundg_z14_dual"
rm -f "$OUTPUT_DIR/opt_soundg_z14.mbtiles" 2>/dev/null || true
run_test "pg_dual_soundg_z14_opt" \
    $TIPPECANOE_DB -q -f -z14 \
    -P -r5 --drop-densest-as-needed --coalesce-densest-as-needed --simplification=10 \
    --postgis "${POSTGIS_CONN}:sys_ht_1_10_SOUNDG_point:geom" \
    --mongo "${MONGO_CONN}:opt_soundg_z14_dual" \
    -o "$OUTPUT_DIR/opt_soundg_z14.mbtiles"
log "MONGODB_STATS:"
get_mongo_stats "opt_soundg_z14_dual" | tee -a "$LOG_FILE"
log "MBTILES_STATS: $(get_mbtiles_stats "$OUTPUT_DIR/opt_soundg_z14.mbtiles")" | tee -a "$LOG_FILE"
log ""

log "--- 3.4 DEPARE_polygon z14 优化参数 -> MongoDB + MBTiles (对比) ---"
clean_mongo "opt_depare_z14_dual"
rm -f "$OUTPUT_DIR/opt_depare_z14.mbtiles" 2>/dev/null || true
run_test "pg_dual_depare_z14_opt" \
    $TIPPECANOE_DB -q -f -z14 \
    -P -r5 --drop-densest-as-needed --coalesce-densest-as-needed --simplification=10 \
    --postgis "${POSTGIS_CONN}:sys_ht_1_10_DEPARE_polygon:geom" \
    --mongo "${MONGO_CONN}:opt_depare_z14_dual" \
    -o "$OUTPUT_DIR/opt_depare_z14.mbtiles"
log "MONGODB_STATS:"
get_mongo_stats "opt_depare_z14_dual" | tee -a "$LOG_FILE"
log "MBTILES_STATS: $(get_mbtiles_stats "$OUTPUT_DIR/opt_depare_z14.mbtiles")" | tee -a "$LOG_FILE"
log ""

log ""
log "=== PART 4: MongoDB 写入参数调优测试 ==="
log ""

log "--- 4.1 DEPARE_polygon z8 不同 batch_size ---"
for batch in 50 100 200 500; do
    clean_mongo "batch_depare_z8_b${batch}"
    run_test "pg_mongo_depare_z8_batch${batch}" \
        $TIPPECANOE_DB -q -f -z8 \
        --postgis "${POSTGIS_CONN}:sys_ht_1_10_DEPARE_polygon:geom" \
        --mongo "${MONGO_CONN}:batch_depare_z8_b${batch}" \
        --mongo-batch-size ${batch}
    log "MONGODB_STATS:"
    get_mongo_stats "batch_depare_z8_b${batch}" | tee -a "$LOG_FILE"
    log ""
done

log "--- 4.2 DEPARE_polygon z8 不同 pool_size ---"
for pool in 1 4 8; do
    clean_mongo "pool_depare_z8_p${pool}"
    run_test "pg_mongo_depare_z8_pool${pool}" \
        $TIPPECANOE_DB -q -f -z8 \
        --postgis "${POSTGIS_CONN}:sys_ht_1_10_DEPARE_polygon:geom" \
        --mongo "${MONGO_CONN}:pool_depare_z8_p${pool}" \
        --mongo-pool-size ${pool}
    log "MONGODB_STATS:"
    get_mongo_stats "pool_depare_z8_p${pool}" | tee -a "$LOG_FILE"
    log ""
done

log ""
log "=== PART 5: 超大数据测试 (sys_ht_mark 599030条) ==="
log ""

log "--- 5.1 sys_ht_mark z5 -> MongoDB ---"
clean_mongo "mark_z5"
run_test "pg_mongo_mark_z5" \
    $TIPPECANOE_DB -q -f -z5 \
    --postgis "${POSTGIS_CONN}:sys_ht_mark:geom" \
    --mongo "${MONGO_CONN}:mark_z5"
log "MONGODB_STATS:"
get_mongo_stats "mark_z5" | tee -a "$LOG_FILE"
log ""

log "--- 5.2 sys_ht_mark z8 -> MongoDB ---"
clean_mongo "mark_z8"
run_test "pg_mongo_mark_z8" \
    $TIPPECANOE_DB -q -f -z8 \
    --postgis "${POSTGIS_CONN}:sys_ht_mark:geom" \
    --mongo "${MONGO_CONN}:mark_z8"
log "MONGODB_STATS:"
get_mongo_stats "mark_z8" | tee -a "$LOG_FILE"
log ""

log "--- 5.3 sys_ht_mark z12 优化 -> MongoDB ---"
clean_mongo "mark_z12_opt"
run_test "pg_mongo_mark_z12_opt" \
    $TIPPECANOE_DB -q -f -z12 \
    -P -r5 --drop-densest-as-needed --coalesce-densest-as-needed \
    --postgis "${POSTGIS_CONN}:sys_ht_mark:geom" \
    --mongo "${MONGO_CONN}:mark_z12_opt"
log "MONGODB_STATS:"
get_mongo_stats "mark_z12_opt" | tee -a "$LOG_FILE"
log ""

log ""
log "=== 测试完成 ==="
log "完整日志: $LOG_FILE"
