#!/usr/bin/env python3
# 质检回归数据生成器：随机点 GeoJSON FeatureCollection。
# 用法：
#   python3 gen_bigpoints.py <输出路径> <特征数> [seed]
# 约定：
#   bigpoints.geojson  —— 40 万特征，P0/P1/P2 checkpoint 回归（run_p1.sh / run_p2.sh 等）
#   hugepoints.geojson —— 1200 万特征，单瓦片超限场景（run_t5/t6/t7，需 -as 收敛）
import json
import math
import random
import sys


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    out_path, count = sys.argv[1], int(sys.argv[2])
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 42

    rng = random.Random(seed)
    # 中国及周边范围，与质检时所用数据分布一致（部分点越过 180° 经线测试环绕处理）
    with open(out_path, "w") as f:
        f.write('{"type":"FeatureCollection","features":[')
        first = True
        for i in range(count):
            lon = rng.uniform(73, 136)
            lat = rng.uniform(18, 54)
            if i % 97 == 0:
                lon = rng.uniform(172, 186)  # 跨反经线样本
            name = "p%d" % i
            pop = rng.randint(0, 100000)
            if not first:
                f.write(",")
            first = False
            f.write(json.dumps({
                "type": "Feature",
                "properties": {"name": name, "population": pop},
                "geometry": {"type": "Point", "coordinates": [round(lon, 6), round(lat, 6)]},
            }, separators=(",", ":")))
        f.write("]}")


if __name__ == "__main__":
    main()
