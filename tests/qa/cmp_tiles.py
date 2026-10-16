import sqlite3, sys
a = sqlite3.connect(sys.argv[1]); b = sqlite3.connect(sys.argv[2])
ka = set(a.execute("SELECT zoom_level,tile_column,tile_row FROM tiles"))
kb = set(b.execute("SELECT zoom_level,tile_column,tile_row FROM tiles"))
print(f"grid: same={ka==kb} (A={len(ka)} B={len(kb)})")
if ka == kb and len(ka) > 0:
    da = dict(((z,x,y),d) for z,x,y,d in a.execute("SELECT zoom_level,tile_column,tile_row,tile_data FROM tiles"))
    db_ = dict(((z,x,y),d) for z,x,y,d in b.execute("SELECT zoom_level,tile_column,tile_row,tile_data FROM tiles"))
    diff = sum(1 for k in da if da[k] != db_[k])
    print(f"bytes diff: {diff}")
