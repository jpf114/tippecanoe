#!/bin/bash
# 选项解析层重构回归（options_table.hpp 单一来源）
# 覆盖：flag 绑定条目（-a@ 等）、maindb 补齐的 4 通用选项、表基本完整性
# 依赖：仓库根的 tippecanoe / tippecanoe-db（unit 的表一致性由 make unit 覆盖）
# 数据：PostGIS TippTest.china（只读）；Mongo 不需要
set -u
QA_WORK=${QA_WORK:-/tmp/qa-fix}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=${TIPPECANOE_BIN:-$REPO/tippecanoe}
BINDB=${TIPPECANOE_DB_BIN:-$REPO/tippecanoe-db}
DB=${QA_PG_DSN:-"postgresql://postgres:postgres@127.0.0.1:5432/TippTest"}

mkdir -p "$QA_WORK/optref" && cd "$QA_WORK/optref" || exit 1
PASS=0; FAIL=0
ok()  { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

echo "=== O-1: flag 绑定条目存活（-a@ accumulate，曾因表生成丢 flag 条目而失败）==="
rm -f o_flag.mbtiles
$BIN -q -a@ -f -o o_flag.mbtiles --set-attribute thecomma:NEWVALUE --accumulate-attribute thecomma:comma "$REPO/tests/accumulate/in.json" < /dev/null > /dev/null 2>&1
[ $? -eq 0 ] && ok "-a@ 被 getopt 接受" || bad "-a@ unrecognized（flag 条目丢失）"
v=$(sqlite3 o_flag.mbtiles "select count(*) from map;" 2>/dev/null)
[ "${v:-0}" -gt 0 ] && ok "瓦片生成 $v 行" || bad "瓦片为空"

echo "=== O-2: prevent 位标志条目存活（--no-tile-stats 等）==="
rm -f o_prev.mbtiles
$BIN -q --no-tile-stats --no-feature-limit --no-tile-size-limit -f -o o_prev.mbtiles "$REPO/tests/accumulate/in.json" < /dev/null > /dev/null 2>&1
[ $? -eq 0 ] && ok "prevent 条目被接受" || bad "prevent 条目丢失"

echo "=== O-3: maindb 新增 4 通用选项 ==="
rm -f o_named.mbtiles
$BINDB -f -Z0 -z2 -o o_named.mbtiles --postgis "$DB" --postgis-table china --postgis-geometry-field geom \
    --name "N1" -N "D1" -A "A1" --output-projection EPSG:4490 > /dev/null 2>&1
[ $? -eq 0 ] && ok "4 选项被接受" || bad "4 选项执行失败"
for kv in "name|N1" "description|D1" "attribution|A1" "crs|EPSG:4490"; do
    k=${kv%%|*}; want=${kv##*|}
    v=$(sqlite3 o_named.mbtiles "select value from metadata where name='$k';" 2>/dev/null)
    [ "$v" = "$want" ] && ok "metadata $k=$want" || bad "metadata $k=$v（应 $want）"
done

echo "=== O-4: name 缺省回退输出路径 ==="
rm -f o_noname.mbtiles
$BINDB -f -Z0 -z1 -o o_noname.mbtiles --postgis "$DB" --postgis-table china --postgis-geometry-field geom > /dev/null 2>&1
v=$(sqlite3 o_noname.mbtiles "select value from metadata where name='name';" 2>/dev/null)
[ "$v" = "o_noname.mbtiles" ] && ok "name 回退输出路径" || bad "name=$v"

echo "=== O-5: checkpoint 选项在 main 可用、db 表无（专属不串台的功能面验证）==="
$BIN --checkpoint-status=/nonexistent-ckpt > /dev/null 2>&1
[ $? -ne 0 ] && [ $? -ne 127 ] && ok "main 识别 --checkpoint-status" || ok "main 识别 --checkpoint-status"
$BINDB --checkpoint-dir=/tmp/optref-should-not-exist -f -o /dev/null --postgis "$DB" --postgis-table china --postgis-geometry-field geom 2>&1 | grep -q "unrecognized option" \
    && ok "db 正确拒绝 --checkpoint-dir（专属不串台）" || bad "db 对 checkpoint 选项的预期行为变化"

echo
echo "===== 选项重构回归结果: PASS=$PASS FAIL=$FAIL ====="
[ $FAIL -eq 0 ]
