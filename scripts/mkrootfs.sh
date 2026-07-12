#!/bin/bash
# LestraOS — build the ext2 root filesystem image from sysroot/.
#
# Usage: ./mkrootfs.sh [output.img]
#
# Output: lestraos-root.ext2 (default) — a ~16 MiB ext2 image.

set -e

OUT="${1:-build/lestraos-root.ext2}"
SIZE_MB=16

cd "$(dirname "$0")/.."

mkdir -p build

# Run the Python builder (uses genext2fs if available, falls back to
# mke2fs).
python3 scripts/mkext2.py sysroot "$OUT" "$SIZE_MB"

echo "mkrootfs: $OUT created ($(stat -c%s "$OUT") bytes)"
