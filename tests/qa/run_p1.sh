#!/bin/bash
QA_WORK=${QA_WORK:-/tmp/qa-fix}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=${TIPPECANOE_BIN:-$REPO/tippecanoe}
BP=/tmp/qa-test/bigpoints.geojson
echo "=== P1 回归：mbtiles ckpt+resume（v4 格式） ==="
rm -rf p1; mkdir -p p1
$BIN $BP -o p1/oneshot.mbtiles -Z0 -z6 -f > p1/oneshot.log 2>&1; echo "oneshot exit=$?"
$BIN $BP -o p1/ck.mbtiles --checkpoint-dir=p1/ckpt -Z0 -z6 -f > p1/fresh.log 2>&1 &
FPID=$!; sleep 8; kill -TERM $FPID 2>/dev/null; wait $FPID; echo "fresh exit=$?"
python3 -c "
import json
s=json.load(open('p1/ckpt/state.json'))
print('format_version=%s last_completed=%s' % (s.get('format_version'), s.get('tiling',{}).get('last_completed_zoom')))
"
ls -la p1/ckpt/blobs/ | awk '{print $5, $9}' | tail -6
$BIN --resume=p1/ckpt > p1/resume.log 2>&1; echo "resume exit=$?"
grep -E "cleaned up|restored" p1/resume.log | head -2
python3 /tmp/qa-fix/cmp_tiles.py p1/oneshot.mbtiles p1/ck.mbtiles
echo ""
echo "=== CK-S7/cpus 校验负向测试：伪造 state.json 的 temp_files ==="
rm -rf p1b; mkdir -p p1b
$BIN $BP -o p1b/ck.mbtiles --checkpoint-dir=p1b/ckpt -Z0 -z6 -f > p1b/fresh.log 2>&1 &
FPID=$!; sleep 8; kill -TERM $FPID 2>/dev/null; wait $FPID
python3 -c "
import json
p='p1b/ckpt/state.json'
s=json.load(open(p))
s['temp_files'] = 999
json.dump(s, open(p,'w'))
print('temp_files forged to 999')
"
$BIN --resume=p1b/ckpt > p1b/resume.log 2>&1; echo "resume exit=$? (期望121)"
grep -E "temp files" p1b/resume.log | head -1
echo ""
echo "=== CK-S5 负向测试：破坏 skip_children.bin CRC ==="
rm -rf p1c; mkdir -p p1c
$BIN $BP -o p1c/ck.mbtiles --checkpoint-dir=p1c/ckpt -Z0 -z6 -f > p1c/fresh.log 2>&1 &
FPID=$!; sleep 8; kill -TERM $FPID 2>/dev/null; wait $FPID
ls p1c/ckpt/blobs/ | head -8
if [ -f p1c/ckpt/blobs/strategies.bin ]; then
  python3 -c "
d = bytearray(open('p1c/ckpt/blobs/strategies.bin','rb').read())
d[2] ^= 0xFF
open('p1c/ckpt/blobs/strategies.bin','wb').write(bytes(d))
print('strategies.bin corrupted')"
  $BIN --resume=p1c/ckpt > p1c/resume.log 2>&1; echo "resume exit=$? (期望121)"
  grep -E "CRC" p1c/resume.log | head -1
else echo "no strategies.bin (blob list above)"; fi
