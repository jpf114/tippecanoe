#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INPUT_GEOJSON="${SCRIPT_DIR}/china_country.geojson"
OUTPUT_GEOJSON="${SCRIPT_DIR}/big_data_200g.geojson"
GEN_SCRIPT="${SCRIPT_DIR}/generate_big_data.py"

echo "============================================"
echo "  Big Data Generator for Tippecanoe Testing"
echo "============================================"
echo ""
echo "Input:  ${INPUT_GEOJSON}"
echo "Output: ${OUTPUT_GEOJSON}"
echo "Target: 200 GB / ~1B features"
echo ""

if [ ! -f "${INPUT_GEOJSON}" ]; then
    echo "ERROR: Input file not found: ${INPUT_GEOJSON}"
    exit 1
fi

if [ ! -f "${GEN_SCRIPT}" ]; then
    echo "ERROR: generate_big_data.py not found: ${GEN_SCRIPT}"
    exit 1
fi

# Try python3 first, fallback to python (for Python 2.7 systems)
if command -v python3 &>/dev/null; then
    PYTHON=python3
elif command -v python &>/dev/null; then
    PYTHON=python
else
    echo "ERROR: No Python found. Please install Python 2.7+ or 3.6+"
    exit 1
fi

# Check Python version
PY_VERSION=$($PYTHON -c 'import sys; print(sys.version_info[0])' 2>/dev/null)
echo "Using Python: $($PYTHON --version 2>&1)"

"${PYTHON}" "${GEN_SCRIPT}" \
    -i "${INPUT_GEOJSON}" \
    -o "${OUTPUT_GEOJSON}" \
    --target-gb 200 \
    --subdivide-points 10 \
    --max-coords 6 \
    --scale-min 0.3 \
    --scale-max 2.0 \
    --progress-interval 500000

echo ""
echo "Data generation complete!"
echo "Output file: ${OUTPUT_GEOJSON}"
