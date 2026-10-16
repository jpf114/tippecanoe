#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TIPPECANOE_BIN="${SCRIPT_DIR}/tippecanoe"

INPUT_GEOJSON="${1:-${SCRIPT_DIR}/big_data_200g.geojson}"
OUTPUT_MBILES="${2:-${SCRIPT_DIR}/big_data_200g.mbtiles}"
TEMP_DIR="${SCRIPT_DIR}/tmp_tippecanoe"

echo "============================================"
echo "  Tippecanoe Vector Tiling - Big Data Test"
echo "============================================"
echo ""
echo "Input:    ${INPUT_GEOJSON}"
echo "Output:   ${OUTPUT_MBILES}"
echo "Temp dir: ${TEMP_DIR}"
echo ""

if [ ! -f "${TIPPECANOE_BIN}" ]; then
    echo "ERROR: tippecanoe binary not found at: ${TIPPECANOE_BIN}"
    echo "Please compile tippecanoe first: cd $(dirname ${TIPPECANOE_BIN}) && make -j"
    exit 1
fi

if [ ! -f "${INPUT_GEOJSON}" ]; then
    echo "ERROR: Input GeoJSON not found: ${INPUT_GEOJSON}"
    echo "Please run run_generate.sh first to generate test data."
    exit 1
fi

mkdir -p "${TEMP_DIR}"

echo "Starting tippecanoe tiling..."
echo ""

"${TIPPECANOE_BIN}" \
    -o "${OUTPUT_MBILES}" \
    -f \
    -l big_data \
    -n "Big Data Test 200GB" \
    -z14 \
    -Z0 \
    --drop-densest-as-needed \
    --extend-zooms-if-still-dropping \
    --simplification=15 \
    --buffer=64 \
    --maximum-tile-bytes=50000000 \
    --maximum-tile-features=1000000 \
    --low-detail=6 \
    --read-parallel \
    --temporary-directory="${TEMP_DIR}" \
    --progress-interval=30 \
    "${INPUT_GEOJSON}"

echo ""
echo "Tiling complete!"
echo "Output: ${OUTPUT_MBILES}"
echo ""

if [ -f "${OUTPUT_MBILES}" ]; then
    FILESIZE=$(du -h "${OUTPUT_MBILES}" | cut -f1)
    echo "Output file size: ${FILESIZE}"
fi
