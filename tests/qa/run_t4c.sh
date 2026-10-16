#!/bin/bash
QA_WORK=${QA_WORK:-/tmp/qa-fix}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=${TIPPECANOE_BIN:-$REPO/tippecanoe}
DATA=/tmp/qa-fix/hugepoints.geojson
echo "=== T4d: CK-S2 (SIGTERM at 5s -> last_completed=-1 + injected z0 row) ==="
rm -rf t4d; mkdir -p t4d
$BIN $DATA -o t4d/ck.mbtiles --checkpoint-dir=t4d/ckpt -Z0 -z6 -f > t4d/fresh.log 2>&1 &
FPID=$!
sleep 5
kill -TERM $FPID 2>/dev/null
wait $FPID; echo "fresh exit=$?"
python3 -c "
import json
s=json.load(open('t4d/ckpt/state.json'))
t=s.get('tiling',{})
print('state: last_completed=%s entry_snapshot=%s iz=%s' % (t.get('last_completed_zoom'), t.get('entry_snapshot_done'), t.get('iz')))
"
sqlite3 t4d/ck.mbtiles "INSERT OR REPLACE INTO map (zoom_level,tile_column,tile_row,tile_id) VALUES (0,0,0,'fake_injected');"
echo "injected map rows: $(sqlite3 t4d/ck.mbtiles 'SELECT count(*) FROM map;')"
$BIN --resume=t4d/ckpt > t4d/resume.log 2>&1
echo "resume exit=$?"
grep -E "cleaned up|UNIQUE|constraint|No zoom" t4d/resume.log | head -3
echo "map rows after: $(sqlite3 t4d/ck.mbtiles 'SELECT count(*) FROM map;')"
python3 /tmp/qa-fix/cmp_tiles.py t1/oneshot.mbtiles t4d/ck.mbtiles
