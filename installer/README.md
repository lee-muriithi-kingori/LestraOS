# Lestra OS Installation

This directory contains **LestraOS installer tools** that write the disk image
to a target device (USB stick, virtual disk, raw image).

## Quick install

### Windows (using built `install.py`)

```cmd
:: Build the disk image first
cd ..\build
..\installer\install.py --target build\install.vhd --image lestraos.img --auto
```

Then attach `install.vhd` in Oracle VirtualBox.

### Linux / macOS (using `install.sh`)

```bash
cd installer
./install.sh --target build/install.img --image ../build/lestraos.img --auto
```

### Real hardware

On Linux, you can write directly to a USB stick:

```bash
sudo ./install.sh --target /dev/sdb --image ../build/lestraos.img
```

Then boot the target machine from that USB stick. The first 512-byte MBR
stage1 reads the rest of the disk into memory and hands off to the
LestraOS kernel.

## Files

| File | Purpose |
|------|---------|
| `install.py` | Python host-side installer. Uses raw I/O on Windows and `dd` on POSIX. |
| `install.sh` | POSIX bash installer (requires `dd`, optional `sgdisk`). |
| `install.c` | In-kernel installer stub. Future: install-payload from remote. |
| `README.md` | (this file) |

## What gets written

The LestraOS image is a 2 MiB raw disk with this layout:

```
+--------------------+ LBA 0
|   Stage1 (MBR)     |  512 bytes
+--------------------+ LBA 1
|  Kernel (146 KB)   |
+--------------------+ LBA 147
|  (zero padding)    |
+--------------------+ LBA 4096
```

Stage1 sets up a flat 4 GB GDT, switches to 32-bit protected mode, then
jumps into the kernel which itself sets up long mode page tables and a
proper 64-bit GDT before entering kernel_main.

## Backing up an existing OS

This installer **overwrites the entire target**. Backup any data on the
target device before running. Pass `--auto` only after confirming the target.
