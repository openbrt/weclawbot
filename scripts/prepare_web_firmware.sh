#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build}"
OUT_DIR="${OUT_DIR:-$PROJECT_DIR/web/firmware}"

BOOTLOADER="$BUILD_DIR/bootloader/bootloader.bin"
APP="$BUILD_DIR/weclawbot.bin"
PARTITIONS="$BUILD_DIR/partition_table/partition-table.bin"
OTADATA="$BUILD_DIR/ota_data_initial.bin"

for file in "$BOOTLOADER" "$APP" "$PARTITIONS" "$OTADATA"; do
    if [[ ! -f "$file" ]]; then
        echo "Missing build artifact: $file" >&2
        echo "Run ./scripts/idf.sh build first." >&2
        exit 1
    fi
done

mkdir -p "$OUT_DIR"

cp "$BOOTLOADER" "$OUT_DIR/bootloader.bin"
cp "$PARTITIONS" "$OUT_DIR/partition-table.bin"
cp "$OTADATA" "$OUT_DIR/ota_data_initial.bin"
cp "$APP" "$OUT_DIR/weclawbot.bin"

echo "Prepared web firmware parts in: $OUT_DIR"
echo "NVS at 0x9000 is not written by web/manifest.json."
