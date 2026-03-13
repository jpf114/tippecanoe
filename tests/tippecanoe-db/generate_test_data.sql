-- tippecanoe-db 参数验证测试数据生成脚本
-- 用于创建 PostGIS 测试表和索引

-- 启用 PostGIS 扩展
CREATE EXTENSION IF NOT EXISTS postgis;

-- 删除已存在的测试表
DROP TABLE IF EXISTS test_points CASCADE;
DROP TABLE IF EXISTS test_lines CASCADE;
DROP TABLE IF EXISTS test_polygons CASCADE;
DROP TABLE IF EXISTS test_mixed CASCADE;

-- 创建点要素测试表 (10,000 个点)
CREATE TABLE test_points AS 
SELECT 
    generate_series as id, 
    ST_SetSRID(ST_MakePoint(random()*360-180, random()*180-90), 4326) as geom,
    'point_' || generate_series as name,
    CASE 
        WHEN generate_series % 10 = 0 THEN 'category_a'
        WHEN generate_series % 10 = 1 THEN 'category_b'
        WHEN generate_series % 10 = 2 THEN 'category_c'
        ELSE 'other'
    END as category,
    random() as value,
    (random() * 1000)::integer as population,
    (random() > 0.5) as active,
    now() - (random() * interval '365 days') as created_date
FROM generate_series(1, 10000);

-- 创建线要素测试表 (1,000 条线)
CREATE TABLE test_lines AS 
SELECT 
    generate_series as id,
    ST_SetSRID(ST_MakeLine(
        ST_MakePoint(random()*360-180, random()*180-90),
        ST_MakePoint(random()*360-180, random()*180-90)
    ), 4326) as geom,
    'line_' || generate_series as name,
    CASE 
        WHEN generate_series % 5 = 0 THEN 'highway'
        WHEN generate_series % 5 = 1 THEN 'street'
        WHEN generate_series % 5 = 2 THEN 'path'
        ELSE 'other'
    END as type,
    random() * 100 as length_km,
    (random() * 10)::integer as lanes,
    (random() > 0.7) as toll
FROM generate_series(1, 1000);

-- 创建面要素测试表 (500 个多边形)
CREATE TABLE test_polygons AS 
SELECT 
    generate_series as id,
    ST_SetSRID(ST_Buffer(ST_MakePoint(random()*360-180, random()*180-90), random()*0.1+0.01), 4326) as geom,
    'polygon_' || generate_series as name,
    CASE 
        WHEN generate_series % 4 = 0 THEN 'residential'
        WHEN generate_series % 4 = 1 THEN 'commercial'
        WHEN generate_series % 4 = 2 THEN 'industrial'
        ELSE 'agricultural'
    END as landuse,
    ST_Area(ST_Transform(
        ST_Buffer(ST_MakePoint(random()*360-180, random()*180-90), random()*0.1+0.01), 
        3857
    )) as area_sqm,
    (random() * 10000)::integer as population,
    (random() * 50)::integer / 100.0 as density
FROM generate_series(1, 500);

-- 创建混合要素测试表 (包含点、线、面)
CREATE TABLE test_mixed AS 
SELECT id, name, 'Point'::text as geometry_type, geom, category, value, population
FROM test_points WHERE id <= 1000
UNION ALL
SELECT id, name, 'Line'::text as geometry_type, geom, type as category, length_km as value, lanes as population
FROM test_lines WHERE id <= 500
UNION ALL
SELECT id, name, 'Polygon'::text as geometry_type, geom, landuse as category, area_sqm as value, population
FROM test_polygons WHERE id <= 250;

-- 创建空间索引
CREATE INDEX idx_points_geom ON test_points USING GIST(geom);
CREATE INDEX idx_lines_geom ON test_lines USING GIST(geom);
CREATE INDEX idx_polygons_geom ON test_polygons USING GIST(geom);
CREATE INDEX idx_mixed_geom ON test_mixed USING GIST(geom);

-- 创建属性索引
CREATE INDEX idx_points_category ON test_points(category);
CREATE INDEX idx_points_value ON test_points(value);
CREATE INDEX idx_lines_type ON test_lines(type);
CREATE INDEX idx_polygons_landuse ON test_polygons(landuse);

-- 创建统计信息
ANALYZE test_points;
ANALYZE test_lines;
ANALYZE test_polygons;
ANALYZE test_mixed;

-- 显示测试表统计信息
SELECT 
    'test_points' as table_name, 
    count(*) as feature_count, 
    ST_GeometryType(geom) as geometry_type
FROM test_points
GROUP BY ST_GeometryType(geom);

SELECT 
    'test_lines' as table_name, 
    count(*) as feature_count, 
    ST_GeometryType(geom) as geometry_type
FROM test_lines
GROUP BY ST_GeometryType(geom);

SELECT 
    'test_polygons' as table_name, 
    count(*) as feature_count, 
    ST_GeometryType(geom) as geometry_type
FROM test_polygons
GROUP BY ST_GeometryType(geom);

SELECT 
    'test_mixed' as table_name, 
    count(*) as feature_count, 
    geometry_type,
    count(*) as type_count
FROM test_mixed
GROUP BY geometry_type;

-- 显示空间范围
SELECT 
    'test_points' as table_name,
    ST_XMin(ST_Extent(geom)) as min_lon,
    ST_YMin(ST_Extent(geom)) as min_lat,
    ST_XMax(ST_Extent(geom)) as max_lon,
    ST_YMax(ST_Extent(geom)) as max_lat
FROM test_points
UNION ALL
SELECT 
    'test_lines' as table_name,
    ST_XMin(ST_Extent(geom)) as min_lon,
    ST_YMin(ST_Extent(geom)) as min_lat,
    ST_XMax(ST_Extent(geom)) as max_lon,
    ST_YMax(ST_Extent(geom)) as max_lat
FROM test_lines
UNION ALL
SELECT 
    'test_polygons' as table_name,
    ST_XMin(ST_Extent(geom)) as min_lon,
    ST_YMin(ST_Extent(geom)) as min_lat,
    ST_XMax(ST_Extent(geom)) as max_lon,
    ST_YMax(ST_Extent(geom)) as max_lat
FROM test_polygons;

-- 创建测试视图（用于测试 SQL 查询参数）
CREATE OR REPLACE VIEW test_points_view AS
SELECT 
    id,
    name,
    category,
    value,
    population,
    active,
    ST_AsGeoJSON(geom) as geojson
FROM test_points
WHERE active = true AND population > 500;

-- 创建测试函数（用于测试复杂查询）
CREATE OR REPLACE FUNCTION get_test_points_by_category(cat text)
RETURNS TABLE(
    id integer,
    name text,
    category text,
    value double precision,
    geojson text
) AS $$
BEGIN
    RETURN QUERY
    SELECT 
        tp.id,
        tp.name,
        tp.category,
        tp.value,
        ST_AsGeoJSON(tp.geom) as geojson
    FROM test_points tp
    WHERE tp.category = cat;
END;
$$ LANGUAGE plpgsql;

-- 输出测试数据摘要
SELECT '测试数据生成完成！' as status;
SELECT '================================' as separator;
SELECT '测试表统计信息:' as info;
SELECT 
    schemaname,
    tablename,
    n_live_tup as row_count
FROM pg_stat_user_tables
WHERE tablename IN ('test_points', 'test_lines', 'test_polygons', 'test_mixed')
ORDER BY tablename;
