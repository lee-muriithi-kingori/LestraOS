#!/bin/bash
# Lestra OS - Create Initial Ramdisk
# Copyright (c) 2026 lestramk.org

USER_DIR="${1:-build/user}"
OUTPUT="${2:-build/initrd.img}"

echo "Creating initrd..."

# Create a simple initrd structure
mkdir -p /tmp/initrd
cp "$USER_DIR"/* /tmp/initrd/ 2>/dev/null || true

# Create initrd header and pack files
python3 << 'EOF'
import os
import struct
import sys

src_dir = "/tmp/initrd"
output = sys.argv[1] if len(sys.argv) > 1 else "build/initrd.img"

files = []
for f in sorted(os.listdir(src_dir)):
    path = os.path.join(src_dir, f)
    if os.path.isfile(path):
        with open(path, "rb") as fp:
            data = fp.read()
        files.append((f, data))

# Calculate offsets
header_size = 8 + len(files) * 80  # header + file entries
data_offset = header_size

# Write initrd
with open(output, "wb") as out:
    # Header: magic + num_files + data_offset
    out.write(b"LRD\x00")
    out.write(struct.pack("<I", len(files)))
    out.write(struct.pack("<I", data_offset))
    
    # File entries
    current_offset = 0
    for name, data in files:
        name_bytes = name.encode("utf-8")[:63]
        entry = bytearray(80)
        entry[:len(name_bytes)] = name_bytes
        entry[64:68] = struct.pack("<I", current_offset)
        entry[68:72] = struct.pack("<I", len(data))
        entry[72:74] = struct.pack("<H", 0o755)
        entry[74:76] = struct.pack("<H", 1)  # regular file
        out.write(entry)
        current_offset += len(data)
    
    # File data
    for name, data in files:
        out.write(data)

print(f"initrd created: {len(files)} files")
EOF

rm -rf /tmp/initrd
echo "initrd created at $OUTPUT"
