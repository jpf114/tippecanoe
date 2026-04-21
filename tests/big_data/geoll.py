import json
import ijson
from decimal import Decimal

# 自定义处理 Decimal 的编码器
def default(obj):
    if isinstance(obj, Decimal):
        return float(obj)
    raise TypeError(f"Object of type {obj.__class__.__name__} is not JSON serializable")

# 文件路径
input_geojson = "California.geojson"
output_geojsonl = "California.geojsonl"

print("开始转换 GeoJSON -> GeoJSONSeq（已支持 Decimal）...")

with open(input_geojson, 'r', encoding='utf-8') as f_in, \
     open(output_geojsonl, 'w', encoding='utf-8') as f_out:

    # 流式读取，不爆内存
    features = ijson.items(f_in, 'features.item')
    
    for idx, feat in enumerate(features):
        # 用 default=default 修复 Decimal 错误
        line = json.dumps(feat, ensure_ascii=False, default=default)
        f_out.write(line + '\n')
        
        # 打印进度
        if idx % 100000 == 0:
            print(f"已处理：{idx} 个要素")

print("✅ 转换完成！输出文件：", output_geojsonl)