#!/bin/bash
QA_WORK=${QA_WORK:-/tmp/qa-fix}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
DB="postgresql://postgres:postgres@localhost:5432/TippTest"
MG="mongodb://admin:admin@localhost:27017/test?authSource=admin"

echo "=== T-DB9 (DB-S5): 重跑不 drop → E11000 幂等成功 ==="
${TIPPECANOE_DB_BIN:-$REPO/tippecanoe-db} --postgis "$DB" --postgis-table china --postgis-geometry-field geom --mongo "$MG&collection=ctid_test" -o /tmp/qa-fix/rerun_p1.mbtiles -Z0 -z6 -f > /tmp/qa-fix/tdb9.log 2>&1
echo "exit=$? (期望0)"
grep -E "already present|Duplicate tiles" /tmp/qa-fix/tdb9.log | head -2
grep -E "Total errors|Total tiles discarded" /tmp/qa-fix/tdb9.log | head -2

echo ""
echo "=== T-DB10 (DB-M1): SQL 模式不显式 geometry-field → 无 geom 泄漏 ==="
${TIPPECANOE_DB_BIN:-$REPO/tippecanoe-db} --postgis "$DB" --postgis-sql 'SELECT id, name, adcode, geom FROM china' --mongo "$MG&collection=chinam1" -o /tmp/qa-fix/m1.mbtiles -Z0 -z8 --mongo-drop-collection -f > /tmp/qa-fix/tdb10.log 2>&1
echo "exit=$?"
python3 - <<'PYEOF'
import sqlite3
a = sqlite3.connect('/tmp/qa-test/china_db4.mbtiles')
b = sqlite3.connect('/tmp/qa-fix/m1.mbtiles')
ka = set(a.execute("SELECT zoom_level,tile_column,tile_row FROM tiles"))
kb = set(b.execute("SELECT zoom_level,tile_column,tile_row FROM tiles"))
da = {(z,x,y):d for z,x,y,d in a.execute("SELECT zoom_level,tile_column,tile_row,tile_data FROM tiles")}
db_ = {(z,x,y):d for z,x,y,d in b.execute("SELECT zoom_level,tile_column,tile_row,tile_data FROM tiles")}
diff = sum(1 for k in da if da[k] != db_[k])
print(f"对比显式 geometry-field 版: grid_same={ka==kb} ({len(ka)}/{len(kb)}) bytes_diff={diff}")
import gzip
raw = gzip.decompress(da[(0,0,0)])
print("z0 含 geom 属性:", b'"geom"' in raw or b'geom' in raw)
PYEOF

echo ""
echo "=== T-DB12 (DB-S8): 混合 SRID 明确报错 ==="
PGPASSWORD=postgres psql -h localhost -U postgres -d TippTest -q <<'EOF'
DROP TABLE IF EXISTS mixed_srid_test;
CREATE TABLE mixed_srid_test (id serial PRIMARY KEY, geom geometry);
INSERT INTO mixed_srid_test (geom) VALUES (ST_SetSRID(ST_MakeEnvelope(100,30,101,31,4326), 4326));
INSERT INTO mixed_srid_test (geom) VALUES (ST_SetSRID(ST_MakeEnvelope(100,30,101,31,3857), 3857));
EOF
${TIPPECANOE_DB_BIN:-$REPO/tippecanoe-db} --postgis "$DB" --postgis-table mixed_srid_test --postgis-geometry-field geom --mongo "$MG&collection=mixedsrid" -o /tmp/qa-fix/mixed.mbtiles -Z0 -z6 --mongo-drop-collection -f > /tmp/qa-fix/tdb12.log 2>&1
echo "exit=$? (期望非0)"
grep -E "mixed SRID" /tmp/qa-fix/tdb12.log | head -1

echo ""
echo "=== 回归: china 表模式 + gc_test ==="
${TIPPECANOE_DB_BIN:-$REPO/tippecanoe-db} --postgis "$DB" --postgis-table gc_test --postgis-geometry-field geom --postgis-shard-mode none --mongo "$MG&collection=gc_p1" -o /tmp/qa-fix/gc_p1.mbtiles -Z0 -z6 --mongo-drop-collection -f > /dev/null 2>&1
echo "gc exit=$?"
