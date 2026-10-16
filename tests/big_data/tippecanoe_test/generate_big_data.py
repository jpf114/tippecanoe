#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generate large-scale NDGeoJSON test data by subdividing and replicating polygons.
Compatible with Python 2.7 and Python 3.x.
Zero dependencies - only uses Python standard library.
"""
from __future__ import print_function
import argparse
import json
import math
import os
import random
import sys
import time


def _print(msg, flush=False):
    print(msg)
    if flush:
        sys.stdout.flush()


def load_source_polygons(geojson_path):
    with open(geojson_path, "r") as f:
        data = json.load(f)
    polygons = []
    for feat in data.get("features", []):
        geom = feat.get("geometry", {})
        gtype = geom.get("type", "")
        coords = geom.get("coordinates", [])
        if gtype == "Polygon":
            for ring in coords:
                if len(ring) >= 4:
                    polygons.append(ring)
        elif gtype == "MultiPolygon":
            for poly in coords:
                for ring in poly:
                    if len(ring) >= 4:
                        polygons.append(ring)
        elif gtype == "MultiLineString":
            for line in coords:
                if len(line) >= 4:
                    polygons.append(line)
    return polygons


def point_in_polygon(px, py, poly):
    """Ray casting algorithm to check if point is inside polygon."""
    n = len(poly)
    inside = False
    j = n - 1
    for i in range(n):
        xi, yi = poly[i][0], poly[i][1]
        xj, yj = poly[j][0], poly[j][1]
        if ((yi > py) != (yj > py)) and (px < (xj - xi) * (py - yi) / (yj - yi) + xi):
            inside = not inside
        j = i
    return inside


def subdivide_polygon(coords, num_points=10):
    """Subdivide a polygon into smaller irregular pieces using grid-based splitting."""
    if len(coords) < 4:
        return [coords]
    minx = min(c[0] for c in coords)
    maxx = max(c[0] for c in coords)
    miny = min(c[1] for c in coords)
    maxy = max(c[1] for c in coords)
    width = maxx - minx
    height = maxy - miny
    if width == 0 or height == 0:
        return [coords]
    result = []
    n_splits_x = max(2, int(math.sqrt(num_points * (width / max(0.01, height)))))
    n_splits_y = max(2, max(2, num_points // n_splits_x))
    cell_w = width / n_splits_x
    cell_h = height / n_splits_y
    for ix in range(n_splits_x):
        for iy in range(n_splits_y):
            cx = minx + ix * cell_w + cell_w / 2
            cy = miny + iy * cell_h + cell_h / 2
            jitter_x = cell_w * 0.3 * (random.random() - 0.5)
            jitter_y = cell_h * 0.3 * (random.random() - 0.5)
            cx += jitter_x
            cy += jitter_y
            if not point_in_polygon(cx, cy, coords):
                continue
            sz = min(cell_w, cell_h) * (0.3 + 0.4 * random.random())
            nv = random.randint(3, 6)
            sub_coords = []
            for k in range(nv):
                angle = 2 * math.pi * k / nv + random.uniform(-0.3, 0.3)
                dist = sz * (0.5 + 0.5 * random.random())
                sub_coords.append((round(cx + dist * math.cos(angle), 8),
                                   round(cy + dist * math.sin(angle), 8)))
            sub_coords.append(sub_coords[0])
            result.append(sub_coords)
    if not result:
        cx = (minx + maxx) / 2
        cy = (miny + maxy) / 2
        sz = min(width, height) * 0.3
        sub_coords = []
        for k in range(4):
            angle = 2 * math.pi * k / 4
            sub_coords.append((round(cx + sz * math.cos(angle), 8),
                               round(cy + sz * math.sin(angle), 8)))
        sub_coords.append(sub_coords[0])
        result.append(sub_coords)
    return result


def coords_to_template(coords, max_coords=0):
    num = len(coords)
    if max_coords > 0 and num > max_coords:
        step = max(1, num // max_coords)
        coords = coords[::step]
        if coords[-1] != coords[0]:
            coords.append(coords[0])
    cx = sum(c[0] for c in coords) / len(coords)
    cy = sum(c[1] for c in coords) / len(coords)
    norm = [(round(c[0] - cx, 8), round(c[1] - cy, 8)) for c in coords]
    return {"coords": norm, "num_coords": len(norm)}


def format_coord(val):
    s = "%.6f" % val
    if "." in s:
        s = s.rstrip("0").rstrip(".")
    return s


def build_feature_string(template, fid, offset_x, offset_y, scale, cos_r, sin_r, src_idx):
    coords = template["coords"]
    parts = [
        '{"type":"Feature","properties":{"id":', str(fid),
        ',"name":"r', str(src_idx), '_', str(fid),
        '","src":', str(src_idx),
        '},"geometry":{"type":"Polygon","coordinates":[[',
    ]
    first = True
    for nx, ny in coords:
        if not first:
            parts.append(",")
        first = False
        rx = nx * cos_r - ny * sin_r
        ry = nx * sin_r + ny * cos_r
        fx = rx * scale + offset_x
        fy = ry * scale + offset_y
        parts.append("[")
        parts.append(format_coord(fx))
        parts.append(",")
        parts.append(format_coord(fy))
        parts.append("]")
    parts.append("]]}}")
    return "".join(parts)


def main():
    parser = argparse.ArgumentParser(
        description="Generate large-scale NDGeoJSON test data by subdividing and replicating polygons"
    )
    parser.add_argument("-i", "--input", required=True, help="Input GeoJSON file")
    parser.add_argument("-o", "--output", required=True, help="Output NDGeoJSON file path")
    parser.add_argument("--target-gb", type=float, default=150, help="Target data size in GB (default: 150)")
    parser.add_argument("--subdivide-points", type=int, default=10,
                        help="Subdivision density per source polygon (default: 10)")
    parser.add_argument("--max-coords", type=int, default=6,
                        help="Max coordinate points per feature (default: 6)")
    parser.add_argument("--lon-min", type=float, default=-180, help="Min longitude (default: -180)")
    parser.add_argument("--lon-max", type=float, default=180, help="Max longitude (default: 180)")
    parser.add_argument("--lat-min", type=float, default=-85, help="Min latitude (default: -85)")
    parser.add_argument("--lat-max", type=float, default=85, help="Max latitude (default: 85)")
    parser.add_argument("--scale-min", type=float, default=0.3, help="Min scale factor (default: 0.3)")
    parser.add_argument("--scale-max", type=float, default=2.0, help="Max scale factor (default: 2.0)")
    parser.add_argument("--seed", type=int, default=42, help="Random seed (default: 42)")
    parser.add_argument("--progress-interval", type=int, default=500000,
                        help="Progress print interval in features (default: 500000)")
    args = parser.parse_args()

    random.seed(args.seed)
    target_bytes = args.target_gb * 1024 * 1024 * 1024

    _print("[1/3] Loading source polygons from %s..." % args.input, flush=True)
    source_polygons = load_source_polygons(args.input)
    _print("      Loaded %d source polygon rings" % len(source_polygons), flush=True)

    _print("[2/3] Subdividing polygons (density=%d)..." % args.subdivide_points, flush=True)
    seed_polygons = []
    for i, ring in enumerate(source_polygons):
        try:
            subs = subdivide_polygon(ring, num_points=args.subdivide_points)
            seed_polygons.extend(subs)
        except Exception:
            seed_polygons.append(ring)
        if (i + 1) % 500 == 0:
            _print("      Subdivided %d/%d, got %d seed polygons" % (
                i + 1, len(source_polygons), len(seed_polygons)), flush=True)

    _print("      Total seed polygons: %d" % len(seed_polygons), flush=True)

    _print("      Converting to coordinate templates...", flush=True)
    templates = []
    for ring in seed_polygons:
        try:
            t = coords_to_template(ring, max_coords=args.max_coords)
            if t["num_coords"] >= 4:
                templates.append(t)
        except Exception:
            continue

    _print("      %d coordinate templates ready" % len(templates), flush=True)

    if not templates:
        _print("ERROR: No valid templates generated!", flush=True)
        return

    avg_coords = sum(t["num_coords"] for t in templates) / len(templates)
    avg_bytes_est = avg_coords * 20 + 80
    total_features_est = int(target_bytes / avg_bytes_est)
    _print("      Avg coords/template: %.1f" % avg_coords, flush=True)
    _print("      Est avg bytes/feature: %.0f" % avg_bytes_est, flush=True)
    _print("      Estimated features needed: %d" % total_features_est, flush=True)

    _print("[3/3] Generating data to %s..." % args.output, flush=True)
    out_dir = os.path.dirname(os.path.abspath(args.output))
    if out_dir:
        try:
            os.makedirs(out_dir)
        except OSError:
            pass

    fid = 0
    total_bytes = 0
    start_time = time.time()
    num_templates = len(templates)

    buf = []
    buf_size = 0
    FLUSH_THRESHOLD = 32 * 1024 * 1024

    with open(args.output, "w") as f:
        while total_bytes < target_bytes:
            for seed_idx in range(num_templates):
                if total_bytes >= target_bytes:
                    break

                template = templates[seed_idx]
                offset_x = random.uniform(args.lon_min, args.lon_max)
                offset_y = random.uniform(args.lat_min, args.lat_max)
                scale = random.uniform(args.scale_min, args.scale_max)
                angle = random.uniform(0, 2 * math.pi)
                cos_r = math.cos(angle)
                sin_r = math.sin(angle)

                line = build_feature_string(
                    template, fid, offset_x, offset_y, scale, cos_r, sin_r, seed_idx
                )
                line += "\n"
                line_bytes = len(line)

                buf.append(line)
                buf_size += line_bytes
                total_bytes += line_bytes
                fid += 1

                if buf_size >= FLUSH_THRESHOLD:
                    f.write("".join(buf))
                    f.flush()
                    buf = []
                    buf_size = 0

                if fid % args.progress_interval == 0:
                    elapsed = time.time() - start_time
                    rate_mb = total_bytes / elapsed / 1024 / 1024
                    pct = total_bytes / target_bytes * 100
                    eta_min = (target_bytes - total_bytes) / (total_bytes / elapsed) / 60 if elapsed > 0 else 0
                    _print(
                        "      Features: %d | %.2f GB | "
                        "%.1f MB/s | %.1f%% | ETA: %.0f min" % (
                            fid, total_bytes / 1024 / 1024 / 1024,
                            rate_mb, pct, eta_min),
                        flush=True)

            if total_bytes >= target_bytes:
                break

        if buf:
            f.write("".join(buf))
            f.flush()
            buf = []

    elapsed = time.time() - start_time
    _print(
        "\nDone! %d features, %.2f GB "
        "in %.1fs (%.1f min)" % (
            fid, total_bytes / 1024 / 1024 / 1024,
            elapsed, elapsed / 60),
        flush=True)
    _print("Output: %s" % args.output, flush=True)


if __name__ == "__main__":
    main()
