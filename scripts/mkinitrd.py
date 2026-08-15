#!/usr/bin/env python3
"""
Lestra OS - Initial Ramdisk Builder
Copyright (c) 2026 lestramk.org

Creates a simple initrd image from a list of files.

Format:
  [u32 num_files]
  For each file:
    [char name[64]]      - null-padded file name
    [u32 file_size]      - size in bytes
    [u8  data[file_size]] - file contents

The kernel's fs/vfs.c:initrd_load() parses this format.
"""

import struct
import sys
import os

MAX_NAME_LEN = 64
MAX_FILES = 64

def build_initrd(output_path, input_files):
    """Build an initrd image from a list of input files."""
    if len(input_files) > MAX_FILES:
        print(f"Warning: too many files ({len(input_files)} > {MAX_FILES}), truncating")
        input_files = input_files[:MAX_FILES]

    with open(output_path, 'wb') as f:
        # Header: number of files
        f.write(struct.pack('<I', len(input_files)))

        for path in input_files:
            if not os.path.exists(path):
                print(f"Warning: file not found: {path}, skipping")
                # Write empty entry
                name = os.path.basename(path).encode('utf-8')[:MAX_NAME_LEN-1]
                name = name.ljust(MAX_NAME_LEN, b'\0')
                f.write(name)
                f.write(struct.pack('<I', 0))
                continue

            # File name (64 bytes, null-padded)
            basename = os.path.basename(path)
            name = basename.encode('utf-8')[:MAX_NAME_LEN-1]
            name = name.ljust(MAX_NAME_LEN, b'\0')
            f.write(name)

            # File size (4 bytes)
            with open(path, 'rb') as inf:
                data = inf.read()
            f.write(struct.pack('<I', len(data)))

            # File data
            f.write(data)

            print(f"  Added {basename} ({len(data)} bytes)")

    total_size = os.path.getsize(output_path)
    print(f"Initrd built: {output_path} ({total_size} bytes, {len(input_files)} files)")

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <output.img> <file1> [file2] ...")
        sys.exit(1)

    output_path = sys.argv[1]
    input_files = sys.argv[2:]
    build_initrd(output_path, input_files)

if __name__ == '__main__':
    main()
