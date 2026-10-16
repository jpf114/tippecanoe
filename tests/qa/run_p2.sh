#!/bin/bash
QA_WORK=${QA_WORK:-/tmp/qa-fix}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BIN=${TIPPECANOE_BIN:-$REPO/tippecanoe}
BP=/tmp/qa-test/bigpoints.geojson
PASS=0; FAIL=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

# v3.4 (M13) 之后 SIGTERM 在读入阶段即生效（exit 110、无快照），
# 定点中断必须两阶段轮询：先等 entry_snapshot_done=true，再等首个 zoom 提交
wait_zoom_committed() {  # $1=ckpt dir  $2=timeout_sec
  local i=0
  while [ $i -lt "$2" ]; do
    python3 - "$1" <<'PYEOF2' && return 0
import json, sys
try:
    s = json.load(open(sys.argv[1] + '/state.json'))
    t = s.get('tiling', {})
    if t.get('entry_snapshot_done') and t.get('last_completed_zoom', -1) >= 0:
        sys.exit(0)
except Exception:
    pass
sys.exit(1)
PYEOF2
    sleep 1; i=$((i+1))
  done
  return 1
}

echo "=== R1: mbtiles ckpt+resume 等价性 + M1 metadata name ==="
rm -rf p2; mkdir -p p2
$BIN $BP -o p2/oneshot.mbtiles -Z0 -z6 -f > p2/oneshot.log 2>&1; echo "  oneshot exit=$?"
$BIN $BP -o p2/ck.mbtiles --checkpoint-dir=p2/ckpt -Z0 -z6 -f > p2/fresh.log 2>&1 &
FPID=$!
wait_zoom_committed p2/ckpt 300 && kill -TERM $FPID 2>/dev/null
wait $FPID; echo "  fresh(SIGTERM after z-commit) exit=$?"
grep -o "state_crc" p2/ckpt/state.json | head -1 | grep -q state_crc && ok "M5 state.json 含 state_crc 字段" || bad "M5 state.json 缺 state_crc"
$BIN --resume=p2/ckpt > p2/resume.log 2>&1; echo "  resume exit=$?"
python3 /tmp/qa-fix/cmp_tiles.py p2/oneshot.mbtiles p2/ck.mbtiles
N_ONESHOT=$(sqlite3 p2/oneshot.mbtiles "SELECT value FROM metadata WHERE name='name'")
N_CK=$(sqlite3 p2/ck.mbtiles "SELECT value FROM metadata WHERE name='name'")
D_ONESHOT=$(sqlite3 p2/oneshot.mbtiles "SELECT value FROM metadata WHERE name='description'")
D_CK=$(sqlite3 p2/ck.mbtiles "SELECT value FROM metadata WHERE name='description'")
echo "  name oneshot='$N_ONESHOT' resume='$N_CK'"
# M1 精确保真：resume 后 name == 原作业 -o 的 as-typed 值（fresh 语义）
[ "$N_CK" = "p2/ck.mbtiles" ] && ok "M1 resume 后 metadata.name 精确恢复 as-typed 路径" || bad "M1 name='$N_CK'（期望 p2/ck.mbtiles）"
# description 在无 -N 时 = fname（各自的 as-typed 输出路径），两侧作业输出名不同本就不该相等；
# 断言 resume 侧忠实于自己的输出路径
[ "$D_CK" = "p2/ck.mbtiles" ] && ok "M1 description 忠实恢复 as-typed 路径" || bad "M1 description='$D_CK'（期望 p2/ck.mbtiles）"
grep -q "elapsed" p2/resume.log && ok "M2 resume 进度行存在" || bad "M2 无进度行"

echo "=== R3: M9 initial snapshot 退出码 121 ==="
rm -rf p2b; mkdir -p p2b
$BIN $BP -o p2b/ck.mbtiles --checkpoint-dir=p2b/ckpt -Z0 -z6 -f > p2b/fresh.log 2>&1 &
FPID=$!
wait_zoom_committed p2b/ckpt 300 && kill -TERM $FPID 2>/dev/null
wait $FPID
python3 - <<'PYEOF'
import json
p='p2b/ckpt/state.json'
s=json.load(open(p))
s['tiling']['entry_snapshot_done']=False
s['state_crc']=0  # 手工改字段后 crc 必然失效；本测试只关注 M9 语义，先绕过 crc
json.dump(s, open(p,'w'))
PYEOF
$BIN --resume=p2b/ckpt > p2b/resume.log 2>&1; RC=$?
grep -o "initial snapshot not saved" p2b/resume.log | head -1
[ "$RC" = "121" ] && ok "M9 exit=121（checkpoint 语义）" || bad "M9 exit=$RC（期望 121）"

echo "=== R4: M10 冲突检查前移（无输出副作用） ==="
rm -rf p2c; mkdir -p p2c
cp -r p2b/ckpt p2c/ckpt 2>/dev/null
$BIN --checkpoint-dir=p2c/newckpt --resume=p2c/ckpt -o p2c/out.mbtiles > p2c/log 2>&1; RC=$?
grep -o "cannot be used together" p2c/log | head -1
[ "$RC" = "101" ] && ok "M10 exit=101" || bad "M10 exit=$RC（期望 101）"
[ ! -e p2c/out.mbtiles ] && ok "M10 无输出库副作用（out.mbtiles 未创建）" || bad "M10 out.mbtiles 已被创建"
[ ! -e p2c/newckpt/state.json ] && ok "M10 无 checkpoint 目录副作用" || bad "M10 newckpt 已建立"

echo "=== R5: M5 state.json 完整性（篡改 tiling 字段） ==="
rm -rf p2d; mkdir -p p2d
cp -r p2b/ckpt p2d/ckpt 2>/dev/null
python3 - <<'PYEOF'
import json
p='p2d/ckpt/state.json'
s=json.load(open(p))
s['tiling']['last_completed_zoom'] = 99  # 篡改进度（state_crc 保持不变 → 必被抓）
json.dump(s, open(p,'w'))
PYEOF
$BIN --resume=p2d/ckpt > p2d/resume.log 2>&1; RC=$?
grep -o "state_crc mismatch" p2d/resume.log | head -1
[ "$RC" = "121" ] && ok "M5 篡改被拦截 exit=121" || bad "M5 exit=$RC（期望 121）"

echo "=== R6: M6/M7 blob 截断拦截 ==="
rm -rf p2e; mkdir -p p2e
cp -r p2b/ckpt p2e/ckpt 2>/dev/null
SZ=$(stat -c %s p2e/ckpt/blobs/pool_off.bin)
truncate -s $((SZ-10)) p2e/ckpt/blobs/pool_off.bin
$BIN --resume=p2e/ckpt > p2e/resume.log 2>&1; RC=$?
grep -o "is truncated" p2e/resume.log | head -1
[ "$RC" = "121" ] && ok "M6 截断 blob 被拦截 exit=121" || bad "M6 exit=$RC（期望 121）"

echo "=== R7: M13 SIGTERM 读入阶段响应 ==="
rm -rf p2f; mkdir -p p2f
$BIN $BP -o p2f/out.mbtiles -Z0 -z6 -f > p2f/log 2>&1 &
FPID=$!; sleep 2; kill -TERM $FPID 2>/dev/null; wait $FPID; RC=$?
grep -o "shutdown signal received during input reading" p2f/log | head -1
[ "$RC" = "110" ] && ok "M13 读入阶段 SIGTERM → exit 110" || bad "M13 exit=$RC（期望 110）"

echo "=== R8: M8 高位字节 name 往返 ==="
rm -rf p2g; mkdir -p p2g
NAME=$'tile\x7fset-中'
$BIN $BP -o p2g/out.mbtiles --checkpoint-dir=p2g/ckpt -n "$NAME" -Z0 -z2 -f > p2g/fresh.log 2>&1 &
FPID=$!; sleep 8; kill -TERM $FPID 2>/dev/null; wait $FPID
python3 - <<'PYEOF'
import json
s=json.load(open('p2g/ckpt/state.json', encoding='utf-8'))
n=s.get('name','')
assert n == 'tile\x7fset-中', 'name 往返失败: %r' % n
print('  state.json name 往返字节精确')
PYEOF
[ $? = 0 ] && ok "M8 DEL+高位字节 name 序列化往返精确" || bad "M8 往返失败"

echo ""
echo "===== P2 回归结果: PASS=$PASS FAIL=$FAIL ====="
