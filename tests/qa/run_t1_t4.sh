#!/bin/bash
QA_WORK=${QA_WORK:-/tmp/qa-fix}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=${TIPPECANOE_BIN:-$REPO/tippecanoe}
DATA=/tmp/qa-test/bigpoints.geojson

echo "=== T1: mbtiles 回归（无特殊参数，SIGTERM 中断后 resume，对比 oneshot） ==="
rm -rf t1_*; mkdir -p t1
# oneshot 基准
$BIN $DATA -o t1/oneshot.mbtiles -Z0 -z6 -f > t1/oneshot.log 2>&1
echo "oneshot exit=$?"
# ckpt 中断：启动 8s 后 SIGTERM（读入约 10-30s，命中读入阶段或早期 zoom）
$BIN $DATA -o t1/ck.mbtiles --checkpoint-dir=t1/ckpt -Z0 -z6 -f > t1/fresh.log 2>&1 &
FPID=$!
sleep 8
kill -TERM $FPID 2>/dev/null
wait $FPID; echo "fresh exit=$?"
grep -c "checkpoint.*committed" t1/fresh.log || true
python3 -c "
import json
s=json.load(open('t1/ckpt/state.json'))
t=s.get('tiling',{})
print('state: last_completed=%s entry_snapshot=%s iz=%s' % (t.get('last_completed_zoom'), t.get('entry_snapshot_done'), t.get('iz')))
"
# resume 到完成
$BIN --resume=t1/ckpt > t1/resume.log 2>&1
echo "resume exit=$?"
grep -E "restored tiling options|cleaned up" t1/resume.log | head -3
# 对比
python3 - <<'EOF'
import sqlite3
a = sqlite3.connect('t1/oneshot.mbtiles'); b = sqlite3.connect('t1/ck.mbtiles')
ta = dict(((z,x,y),d) for z,x,y,d in a.execute("SELECT zoom_level,tile_column,tile_row,tile_data FROM tiles"))
tb = dict(((z,x,y),d) for z,x,y,d in b.execute("SELECT zoom_level,tile_column,tile_row,tile_data FROM tiles"))
diff = [k for k in set(ta)&set(tb) if ta[k]!=tb[k]]
print(f"T1 tiles: A={len(ta)} B={len(tb)} common={len(set(ta)&set(tb))} diff={len(diff)}")
ma = dict(a.execute("SELECT name,value FROM metadata")); mb = dict(b.execute("SELECT name,value FROM metadata"))
for k in sorted(set(ma)|set(mb)):
    if ma.get(k)!=mb.get(k): print(f"  meta[{k}]: differs")
EOF

echo ""
echo "=== T4: CK-S2 注入测试（last_completed=-1 + map 假行 → resume 清理） ==="
rm -rf t4_*; mkdir -p t4
# 中断在读入阶段（entry_snapshot_done 可能 true 但无 zoom 完成，map 表空）
$BIN $DATA -o t4/ck.mbtiles --checkpoint-dir=t4/ckpt -Z0 -z6 -f > t4/fresh.log 2>&1 &
FPID=$!
sleep 6
kill -TERM $FPID 2>/dev/null
wait $FPID; echo "fresh exit=$?"
python3 -c "
import json
s=json.load(open('t4/ckpt/state.json'))
t=s.get('tiling',{})
print('state: last_completed=%s entry_snapshot=%s iz=%s' % (t.get('last_completed_zoom'), t.get('entry_snapshot_done'), t.get('iz')))
"
# 注入假瓦片行（模拟首 zoom 部分写入 map 未提交）
sqlite3 t4/ck.mbtiles "INSERT OR REPLACE INTO map (zoom_level,tile_column,tile_row,tile_id) VALUES (0,0,0,'fake_injected');"
echo "injected map rows: $(sqlite3 t4/ck.mbtiles 'SELECT count(*) FROM map;')"
# resume
$BIN --resume=t4/ckpt > t4/resume.log 2>&1
echo "resume exit=$?"
grep -E "cleaned up|UNIQUE|constraint" t4/resume.log | head -3
echo "map rows after: $(sqlite3 t4/ck.mbtiles 'SELECT count(*) FROM map;')"
python3 - <<'EOF'
import sqlite3
a = sqlite3.connect('t1/oneshot.mbtiles'); b = sqlite3.connect('t4/ck.mbtiles')
ta = {k for k,_ in a.execute("SELECT zoom_level,tile_column,tile_row FROM tiles")}
tb = {k for k,_ in b.execute("SELECT zoom_level,tile_column,tile_row FROM tiles")}
print(f"T4 tile grid vs T1 oneshot: same={ta==tb} (A={len(ta)} B={len(tb)})")
EOF
