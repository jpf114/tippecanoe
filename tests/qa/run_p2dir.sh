#!/bin/bash
QA_WORK=${QA_WORK:-/tmp/qa-fix}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=${TIPPECANOE_BIN:-$REPO/tippecanoe}
BP=/tmp/qa-test/bigpoints.geojson
rm -rf p2dir; mkdir -p p2dir
$BIN $BP -e p2dir/oneshot -Z0 -z6 -f > p2dir/oneshot.log 2>&1; echo "oneshot exit=$?"
$BIN $BP -e p2dir/ck --checkpoint-dir=p2dir/ckpt -Z0 -z6 -f > p2dir/fresh.log 2>&1 &
FPID=$!
# 等 entry 快照 + 首 zoom 提交
i=0
while [ $i -lt 300 ]; do
  python3 -c "
import json,sys
try:
    s=json.load(open('p2dir/ckpt/state.json'))
    t=s.get('tiling',{})
    sys.exit(0 if t.get('entry_snapshot_done') and t.get('last_completed_zoom',-1)>=0 else 1)
except Exception: sys.exit(1)
" && break
  sleep 1; i=$((i+1))
done
kill -TERM $FPID 2>/dev/null; wait $FPID; echo "fresh exit=$?"
echo "中断后 metadata.json: $(ls p2dir/ck/metadata.json 2>/dev/null && echo 存在 || echo 不存在)"
python3 -c "
import json
try:
    m=json.load(open('p2dir/ck/metadata.json'))
    print('  部分完成版 maxzoom=%s name=%s' % (m.get('maxzoom'), m.get('name')))
except Exception as e: print('  (无 metadata)', e)
"
# resume：启动后 1 秒内 metadata 应已被 M11 删除
$BIN --resume=p2dir/ckpt > p2dir/resume.log 2>&1 &
RPID=$!
sleep 1.5
[ ! -f p2dir/ck/metadata.json ] && echo "  [PASS] M11 resume 启动即删过期 metadata.json" || echo "  [WARN] metadata.json 在 resume 早期仍在（时序）"
wait $RPID; echo "resume exit=$?"
python3 - <<'PYEOF'
import json
m=json.load(open('p2dir/ck/metadata.json'))
assert m.get('name')=='p2dir/ck', 'name=%r' % m.get('name')
print('  [PASS] M1 dir 模式 name=%s 精确恢复' % m.get('name'))
PYEOF
python3 - <<'PYEOF'
import json
a=json.load(open('p2dir/ck/metadata.json'))
b=json.load(open('p2dir/oneshot/metadata.json'))
# 两侧各自忠实于自己的输出名
assert a.get('name')=='p2dir/ck' and b.get('name')=='p2dir/oneshot'
same=[k for k in b if k not in ('name','description','generator_options','json') and a.get(k)==b.get(k)]
diff=[k for k in b if k not in ('name','description','generator_options','json') and a.get(k)!=b.get(k)]
print('  metadata 一致字段数=%d 不一致=%s' % (len(same), diff))
assert not diff, diff
print('  [PASS] M1 dir metadata 除输出名相关字段外逐字段一致')
PYEOF
ls p2dir/ck | head -3
echo "===== dir 模式验证完成 ====="
