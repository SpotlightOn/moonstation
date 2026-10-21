#!/usr/bin/env bash
# Build 240x240 baseline JPEG slides for the moonstation firmware.
#
# Usage: ./scripts/build_slides.sh <input_dir> [output_dir]
# Default output_dir is ./slides (relative to the project root).

set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <input_dir> [output_dir]" >&2
    exit 1
fi

INPUT_DIR="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_DIR="${2:-${PROJECT_ROOT}/slides}"

if [ ! -d "${INPUT_DIR}" ]; then
    echo "Input directory not found: ${INPUT_DIR}" >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"

shopt -s nullglob nocaseglob
count=0
for src in "${INPUT_DIR}"/*.jpg "${INPUT_DIR}"/*.jpeg; do
    [ -f "$src" ] || continue
    base="$(basename "$src")"
    name="${base%.*}"
    dst="${OUTPUT_DIR}/${name}.jpg"
    magick "$src" \
        -auto-orient \
        -resize 240x240^ \
        -gravity center \
        -extent 240x240 \
        -colorspace sRGB \
        -strip \
        -quality 85 \
        -interlace none \
        "$dst"
    count=$((count + 1))
    echo "wrote ${dst}"
done

echo "Built ${count} slide(s) in ${OUTPUT_DIR}"