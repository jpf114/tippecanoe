#!/bin/bash
# M15 投影回归（EPSG:4490 / 输出网格 / decode·tile-join crs 感知）
# 依赖：仓库根的 tippecanoe / tippecanoe-decode / tile-join，sqlite3
# 数据：脚本自建 tiny GeoJSON（经纬度）与米制（EPSG:3857）两份
set -u
QA_WORK=${QA_WORK:-/tmp/qa-fix}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=${TIPPECANOE_BIN:-$REPO/tippecanoe}
DEC=${TIPPECANOE_DECODE_BIN:-$REPO/tippecanoe-decode}
TJ=${TILE_JOIN_BIN:-$REPO/tile-join}

mkdir -p "$QA_WORK/m15" && cd "$QA_WORK/m15" || exit 1
rm -f tiny.json tiny3857.json pf.sh m15_*.mbtiles prefilter_seen.jsonl

cat > tiny.json <<'EOF'
{"type":"FeatureCollection","features":[
{"type":"Feature","properties":{"name":"a"},"geometry":{"type":"Point","coordinates":[116.4,39.9]}},
{"type":"Feature","properties":{"name":"b"},"geometry":{"type":"Point","coordinates":[121.5,31.2]}},
{"type":"Feature","properties":{"name":"c"},"geometry":{"type":"Point","coordinates":[113.3,23.1]}},
{"type":"Feature","properties":{"name":"poly"},"geometry":{"type":"Polygon","coordinates":[[[110.0,35.0],[115.0,35.0],[115.0,30.0],[110.0,30.0],[110.0,35.0]]]}}
]}
EOF
python3 -c "
import json, math
R = 20037508.34
def m(lon, lat):
    return (lon * R / 180, math.log(math.tan((90 + lat) * math.pi / 360)) * R / math.pi)
pts = [('a', 116.4, 39.9), ('b', 121.5, 31.2), ('c', 113.3, 23.1)]
feats = [{'type':'Feature','properties':{'name':n},'geometry':{'type':'Point','coordinates':list(m(lo, la))}} for n, lo, la in pts]
open('tiny3857.json', 'w').write('{\"type\":\"FeatureCollection\",\"features\":' + json.dumps(feats) + '}')
"
printf '#!/bin/bash\nhead -1 > prefilter_seen.jsonl\ncat\n' > pf.sh && chmod +x pf.sh

PASS=0; FAIL=0
ok()  { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

echo "=== M15-1: 4490 输入的 crs/网格 metadata ==="
$BIN -f -s EPSG:4490 -zg -o m15_4490.mbtiles tiny.json > /dev/null 2>&1
v=$(sqlite3 m15_4490.mbtiles "select value from metadata where name='crs';")
[ "$v" = "EPSG:4490" ] && ok "crs=EPSG:4490" || bad "crs=$v"
v=$(sqlite3 m15_4490.mbtiles "select value from metadata where name='tile_origin_upper_left_x';")
[ "$v" = "-180.000000" ] && ok "线性网格原点 -180" || bad "origin=$v"

echo "=== M15-2: 单瓦片 decode 走线性反投影（39.9 而非 Mercator 78.2）==="
lat=$($DEC m15_4490.mbtiles 0 0 0 2>/dev/null | python3 -c "
import json, sys
d = json.load(sys.stdin)
f = d['features'][0]['features'][0] if 'features' in d['features'][0] else d['features'][0]
print(f['geometry']['coordinates'][1])
")
python3 -c "exit(0 if abs($lat - 39.9) < 0.5 else 1)" && ok "单瓦片 decode lat=$lat" || bad "单瓦片 decode lat=$lat（应约 39.9）"

echo "=== M15-3: 显式 -s EPSG:3857 decode 输出米制（上游语义）==="
v=$($DEC -s EPSG:3857 -x generator m15_4490.mbtiles 0 0 0 2>/dev/null | grep -oE '[0-9]{7,}(\.[0-9]+)?' | head -1)
python3 -c "exit(0 if $v and $v > 1000000 else 1)" && ok "3857 米制输出 $v" || bad "3857 应输出米制，得到 $v"

echo "=== M15-4: 交叉组合 prefilter 精确经纬度 ==="
$BIN -f -s EPSG:3857 --output-projection EPSG:4490 -zg -o m15_cross.mbtiles --prefilter ./pf.sh tiny3857.json > /dev/null 2>&1
grep -q '\[ 116.400000, 39.900000 \]' prefilter_seen.jsonl && ok "prefilter 116.4/39.9 精确" || bad "prefilter 坐标错误"

echo "=== M15-5: tile-join 透传 crs + 瓦片字节不变 ==="
$TJ -f -o m15_join.mbtiles m15_4490.mbtiles > /dev/null 2>&1
v=$(sqlite3 m15_join.mbtiles "select value from metadata where name='crs';")
[ "$v" = "EPSG:4490" ] && ok "tile-join crs 透传" || bad "tile-join crs=$v"
a=$(sqlite3 m15_4490.mbtiles "select zoom_level,tile_column,tile_row,tile_data from tiles order by 1,2,3" | md5sum | cut -d' ' -f1)
b=$(sqlite3 m15_join.mbtiles "select zoom_level,tile_column,tile_row,tile_data from tiles order by 1,2,3" | md5sum | cut -d' ' -f1)
[ "$a" = "$b" ] && ok "瓦片字节不变" || bad "瓦片字节变化"

echo "=== M15-6: 混合网格输入拒绝（exit 101）==="
$BIN -f -zg -o m15_merc.mbtiles tiny.json > /dev/null 2>&1
$TJ -f -o m15_mixed.mbtiles m15_4490.mbtiles m15_merc.mbtiles > /dev/null 2>&1
[ $? -eq 101 ] && ok "混合线性/Mercator 拒绝 exit 101" || bad "混合输入未按 101 拒绝"
$TJ -f -o m15_mixed2.mbtiles m15_merc.mbtiles m15_4490.mbtiles > /dev/null 2>&1
[ $? -eq 101 ] && ok "反向顺序同样拒绝" || bad "反向顺序未拒绝"
$TJ -f -o m15_same.mbtiles m15_4490.mbtiles m15_4490.mbtiles > /dev/null 2>&1
[ $? -eq 0 ] && ok "同网格合并不误伤" || bad "同网格合并被误拒"

echo
echo "===== M15 回归结果: PASS=$PASS FAIL=$FAIL ====="
[ $FAIL -eq 0 ]
