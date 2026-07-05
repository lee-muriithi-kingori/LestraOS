#!/usr/bin/env bash
# Lestra OS Installer — POSIX host-side tool (sh + dd + sgdisk).
# Usage: ./install.sh --target /dev/sdX --image build/lestraos.img

set -e

usage() {
    echo "Usage: $0 --target <device|image> --image <path>"
    echo ""
    echo "Examples:"
    echo "  $0 --target /dev/sdb --image build/lestraos.img"
    echo "  $0 --target build/install.vhd --image build/lestraos.img --auto"
    exit 1
}

TARGET=""
IMAGE=""
AUTO=0

while [ $# -gt 0 ]; do
    case "$1" in
        --target) TARGET="$2"; shift 2 ;;
        --image) IMAGE="$2"; shift 2 ;;
        --auto) AUTO=1; shift ;;
        -h|--help) usage ;;
        *) echo "unknown: $1"; usage ;;
    esac
done

[ -z "$TARGET" ] && usage
[ -z "$IMAGE" ] && usage
[ ! -f "$IMAGE" ] && { echo "FAIL: $IMAGE not found" >&2; exit 1; }

if [ "$AUTO" != "1" ]; then
    echo "This will OVERWRITE $TARGET with $IMAGE."
    echo -n "Press ENTER to continue, Ctrl-C to abort... "
    read _ || true
fi

echo "LESTRA OS INSTALLER"
echo "  source: $IMAGE"
echo "  target: $TARGET"
echo "  size:   $(stat -c%s "$IMAGE" 2>/dev/null || wc -c < "$IMAGE") bytes"

echo "[1/3] Verifying image..."
IMG_SIZE=$(stat -c%s "$IMAGE" 2>/dev/null || wc -c < "$IMAGE")
if [ "$IMG_SIZE" -gt 16777216 ]; then
    echo "  WARN: image is large (>16 MiB); install may be slow"
fi

echo "[2/3] Installing image..."
case "$TARGET" in
    /dev/*)
        # Block device: dd the raw image
        dd if="$IMAGE" of="$TARGET" bs=512 conv=notrunc status=progress
        # Sync disk partitions
        sync
        # Try sgdisk to write a GPT table (only if installed)
        if command -v sgdisk >/dev/null 2>&1; then
            echo "[3/3] Writing GPT partition table..."
            sgdisk --clear "$TARGET" || true
            sgdisk -n 1:2048:+16M -t 1:8300 "$TARGET" || true
        else
            echo "[3/3] (sgdisk not available; image must include its own MBR/GPT)"
        fi
        ;;
    *.img|*.vhd|*.vdi)
        # File: just copy
        cp -f "$IMAGE" "$TARGET"
        echo "[3/3] (file install: copied image to $TARGET)"
        ;;
    *)
        echo "ERROR: unrecognized target $TARGET" >&2
        exit 1
        ;;
esac

echo "DONE. You may now boot from $TARGET."
