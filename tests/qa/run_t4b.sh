#!/bin/bash
QA_WORK=${QA_WORK:-/tmp/qa-fix}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=${TIPPECANOE_BIN:-$REPO/tippecanoe}
DATA=/tmp/qa-test/bigpoints.geojson
echo "=== T4b: CK-S2（SIGTERM 读入阶段 → last_completed=-1 + 注入 z0 假行） ==="
rm -rf t4b; mkdir -p t4b
$BIN $DATA -o t4b/ck.mbtiles --checkpoint-dir=t4b/ckpt -Z0 -z6 -f > t4b/fresh.log 2>&1 &
FPID=$!
sleep 3
kill -TERM $FPID 2>/dev/null
wait $FPID; echo "fresh exit=$?"
python3 -c "
import json
s=json.load(open('t4b/ckpt/state.json'))
t=s.get('tiling',{})
print('state: last_completed=%s entry_snapshot=%s iz=%s' % (t.get('last_completed_zoom'), t.get('entry_snapshot_done'), t.get('iz')))
"
sqlite3 t4b/ck.mbtiles "INSERT OR REPLACE INTO map (zoom_level,tile_column,tile_row,tile_id) VALUES (0,0,0,'fake_injected');"
echo "injected map rows: $(sqlite3 t4b/ck.mbtiles 'SELECT count(*) FROM map;')"
$BIN --resume=t4b/ckpt > t4b/resume.log 2>&1
echo "resume exit=$?"
grep -E "cleaned up|UNIQUE|constraint|No zoom" t4b/resume.log | head -3
echo "map rows after: $(sqlite3 t4b/ck.mbtiles 'SELECT count(*) FROM map;')"
python3 - <<'EOF'
import sqlite3
a = sqlite3.connect('t1/oneshot.mbtiles'); b = sqlite3.connect('t4b/ck.mbtiles')
ta = set(a.execute("SELECT zoom_level,tile_column,tile_row FROM tiles"))
tb = set(b.execute("SELECT zoom_level,tile_column,tile_row FROM tiles"))
print(f"T4b tile grid vs oneshot: same={ta==tb} (A={len(ta)} B={len(tb)})")
if ta==tb:
    da = dict(((z,x,y),d) for z,x,y,d in a.execute("SELECT zoom_level,tile_column,tile_row,tile_data FROM tiles"))
    db_ = dict(((z,x,y),d) for z,x,y,d in b.execute("SELECT zoom_level,tile_column,tile_row,tile_data FROM tiles"))
    diff = sum(1 for k in da if da[k]!=db_[k])
    print(f"T4b tile bytes diff vs oneshot: {diff}")
EOF
