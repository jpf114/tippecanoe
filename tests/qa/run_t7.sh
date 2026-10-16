#!/bin/bash
QA_WORK=${QA_WORK:-/tmp/qa-fix}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=${TIPPECANOE_BIN:-$REPO/tippecanoe}
DATA=/tmp/qa-fix/hugepoints.geojson
echo "=== T7: CK-S2 two-phase poll -> kill -9 in z0 commit window ==="
rm -rf t7; mkdir -p t7
$BIN $DATA -o t7/ck.mbtiles --checkpoint-dir=t7/ckpt -Z0 -z4 -as -f > t7/fresh.log 2>&1 &
FPID=$!
# 阶段1：等 entry_snapshot_done=true
ENTRY=0
for i in $(seq 1 3000); do
  ENTRY=$(python3 -c "
import json
try:
    s=json.load(open('t7/ckpt/state.json'))
    print(1 if s.get('tiling',{}).get('entry_snapshot_done') else 0)
except Exception: print(0)")
  [ "$ENTRY" = "1" ] && break
  kill -0 $FPID 2>/dev/null || break
  sleep 0.3
done
echo "entry_snapshot_done=$ENTRY at iter $i"
# 阶段2：等 staging 非空（z0 on_zoom_complete 开始）
KILLED=0
if [ "$ENTRY" = "1" ]; then
  for i in $(seq 1 6000); do
    if ls t7/ckpt/staging/ 2>/dev/null | grep -q .; then
      kill -9 $FPID; KILLED=1
      echo "staging non-empty -> kill -9 at iter $i"
      break
    fi
    kill -0 $FPID 2>/dev/null || break
    sleep 0.1
  done
fi
wait $FPID 2>/dev/null; echo "killed=$KILLED fresh exit=$?"
python3 -c "
import json
s=json.load(open('t7/ckpt/state.json'))
t=s.get('tiling',{})
print('state: last_completed=%s entry_snapshot=%s iz=%s' % (t.get('last_completed_zoom'), t.get('entry_snapshot_done'), t.get('iz')))
"
echo "map rows before resume: $(sqlite3 t7/ck.mbtiles 'SELECT count(*) FROM map;' 2>/dev/null)"
echo "map zooms: $(sqlite3 t7/ck.mbtiles 'SELECT DISTINCT zoom_level FROM map;' 2>/dev/null | tr '\n' ' ')"
$BIN --resume=t7/ckpt > t7/resume.log 2>&1
echo "resume exit=$?"
tr '\r' '\n' < t7/resume.log | grep -E "cleaned up|UNIQUE|constraint|No zoom" | head -5
python3 - <<'PYEOF'
import sqlite3
a = sqlite3.connect('t5/oneshot.mbtiles'); b = sqlite3.connect('t7/ck.mbtiles')
ka = set(a.execute("SELECT zoom_level,tile_column,tile_row FROM tiles"))
kb = set(b.execute("SELECT zoom_level,tile_column,tile_row FROM tiles"))
print(f"grid vs oneshot: same={ka==kb} (A={len(ka)} B={len(kb)})")
if ka == kb:
    da = dict(((z,x,y),d) for z,x,y,d in a.execute("SELECT zoom_level,tile_column,tile_row,tile_data FROM tiles"))
    db_ = dict(((z,x,y),d) for z,x,y,d in b.execute("SELECT zoom_level,tile_column,tile_row,tile_data FROM tiles"))
    print(f"bytes diff: {sum(1 for k in da if da[k]!=db_[k])}")
PYEOF
