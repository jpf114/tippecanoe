#!/bin/bash
QA_WORK=${QA_WORK:-/tmp/qa-fix}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=${TIPPECANOE_BIN:-$REPO/tippecanoe}
BP=/tmp/qa-test/bigpoints.geojson

echo "=== T2: CK-S1 dir 模式 metadata 修复 ==="
rm -rf t2; mkdir -p t2
$BIN $BP -e t2/oneshot_dir -Z0 -z6 -f > t2/oneshot.log 2>&1
echo "oneshot exit=$?"
$BIN $BP -e t2/ck_dir --checkpoint-dir=t2/ckpt -Z0 -z6 -f > t2/fresh.log 2>&1 &
FPID=$!
sleep 8; kill -TERM $FPID 2>/dev/null; wait $FPID; echo "fresh exit=$?"
echo "--- 中断时 metadata.json 的 maxzoom/center:"
python3 -c "
import json
m=json.load(open('t2/ck_dir/metadata.json'))
print('maxzoom=%s center=%s' % (m.get('maxzoom'), m.get('center')))
"
$BIN --resume=t2/ckpt > t2/resume.log 2>&1
echo "resume exit=$?"
echo "--- resume 后 metadata.json 的 maxzoom/center:"
python3 -c "
import json
m=json.load(open('t2/ck_dir/metadata.json'))
print('maxzoom=%s center=%s' % (m.get('maxzoom'), m.get('center')))
"
echo "--- 与 oneshot metadata 逐字段对比（排除 name/description/generator_options）:"
python3 - <<'PYEOF'
import json
a=json.load(open('t2/oneshot_dir/metadata.json'))
b=json.load(open('t2/ck_dir/metadata.json'))
skip={'name','description','generator_options'}
diffs={k for k in set(a)|set(b) if a.get(k)!=b.get(k) and k not in skip}
print("diff fields:", sorted(diffs) if diffs else "NONE (等价)")
PYEOF
# 空跑再 resume 一次，确认 metadata 保持正确（幂等）
$BIN --resume=t2/ckpt > t2/resume2.log 2>&1
echo "re-resume exit=$?"
python3 -c "
import json
m=json.load(open('t2/ck_dir/metadata.json'))
print('after re-resume: maxzoom=%s' % m.get('maxzoom'))
"

echo ""
echo "=== T3: CK-S4 特征参数（--buffer=12 --gamma=0.8 --simplification=2 -E id:sum） ==="
rm -rf t3; mkdir -p t3
$BIN $BP -o t3/oneshot.mbtiles --buffer=12 --gamma=0.8 --simplification=2 -E id:sum -Z0 -z6 -f > t3/oneshot.log 2>&1
echo "oneshot exit=$?"
$BIN $BP -o t3/ck.mbtiles --checkpoint-dir=t3/ckpt --buffer=12 --gamma=0.8 --simplification=2 -E id:sum -Z0 -z6 -f > t3/fresh.log 2>&1 &
FPID=$!
sleep 8; kill -TERM $FPID 2>/dev/null; wait $FPID; echo "fresh exit=$?"
python3 -c "
import json
s=json.load(open('t3/ckpt/state.json'))
t=s.get('tiling',{})
o=s.get('tiling_options',{})
print('state: last_completed=%s' % t.get('last_completed_zoom'))
print('persisted options: buffer=%s gamma=%s simplification=%s accum=%s' % (o.get('buffer'), o.get('gamma'), o.get('simplification'), o.get('accumulate_attributes')))
"
$BIN --resume=t3/ckpt > t3/resume.log 2>&1
echo "resume exit=$?"
grep -c "restored tiling options" t3/resume.log
python3 /tmp/qa-fix/cmp_tiles.py t3/oneshot.mbtiles t3/ck.mbtiles
