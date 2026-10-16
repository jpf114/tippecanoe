#!/bin/bash
QA_WORK=${QA_WORK:-/tmp/qa-fix}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=${TIPPECANOE_BIN:-$REPO/tippecanoe}
DATA=/tmp/qa-fix/hugepoints.geojson
echo "=== T5: CK-S2 真实窗口（kill -9 in [entry snapshot, z0 commit), -as） ==="
rm -rf t5; mkdir -p t5
# oneshot 基准（-as）
$BIN $DATA -o t5/oneshot.mbtiles -Z0 -z4 -as -f > t5/oneshot.log 2>&1
echo "oneshot exit=$?"
# fresh ckpt
$BIN $DATA -o t5/ck.mbtiles --checkpoint-dir=t5/ckpt -Z0 -z4 -as -f > t5/fresh.log 2>&1 &
FPID=$!
# 轮询：entry_snapshot_done=true 后，再轮询 map 行数>0 立即 kill -9
for i in $(seq 1 400); do
  sleep 0.5
  DONE=$(python3 -c "
import json
try:
    s=json.load(open('t5/ckpt/state.json'))
    print(1 if s.get('tiling',{}).get('entry_snapshot_done') else 0)
except Exception:
    print(0)
")
  if [ "$DONE" = "1" ]; then
    ROWS=$(sqlite3 t5/ck.mbtiles 'SELECT count(*) FROM map;' 2>/dev/null || echo 0)
    if [ "$ROWS" != "0" ] && [ -n "$ROWS" ]; then
      echo "hit window: map rows=$ROWS, sending kill -9"
      kill -9 $FPID
      break
    fi
  fi
  if ! kill -0 $FPID 2>/dev/null; then echo "process exited before window (rows=$ROWS)"; break; fi
done
wait $FPID 2>/dev/null; echo "fresh killed, exit=$?"
python3 -c "
import json
s=json.load(open('t5/ckpt/state.json'))
t=s.get('tiling',{})
print('state: last_completed=%s entry_snapshot=%s iz=%s' % (t.get('last_completed_zoom'), t.get('entry_snapshot_done'), t.get('iz')))
"
echo "map rows before resume: $(sqlite3 t5/ck.mbtiles 'SELECT count(*) FROM map;' 2>/dev/null)"
$BIN --resume=t5/ckpt > t5/resume.log 2>&1
echo "resume exit=$?"
tr '\r' '\n' < t5/resume.log | grep -E "cleaned up|UNIQUE|constraint|committed|No zoom" | head -8
python3 /tmp/qa-fix/cmp_tiles.py t5/oneshot.mbtiles t5/ck.mbtiles
