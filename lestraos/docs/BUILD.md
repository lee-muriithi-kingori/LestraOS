# Building Lestra OS

## Prerequisites

### Required Tools

| Tool | Version | Purpose |
|------|---------|---------|
| x86_64-elf-gcc | 13.x | Cross compiler (recommended) |
| gcc | 13.x+ | System compiler (fallback) |
| nasm | 2.16+ | Assembler |
| make | 4.x | Build system |
| qemu-system-x86_64 | 8.x | Emulator |
| grub-mkrescue | 2.12 | ISO creation |
| xorriso | 1.5+ | ISO tool |
| python3 | 3.8+ | Initrd builder |

### Ubuntu/Debian

```bash
sudo apt update
sudo apt install build-essential nasm qemu-system-x86 grub-pc-bin xorriso python3
```

### Build Cross Compiler (recommended)

The cross-compiler ensures the kernel is built without any system libc
dependencies. To build it:

```bash
./build/cross-compiler.sh
export PATH=$HOME/opt/cross/bin:$PATH
```

This downloads and builds binutils 2.42 + gcc 13.2.0 targeted at
`x86_64-elf`. Installation goes to `~/opt/cross/`.

If you skip this step, the Makefile falls back to the system `gcc`,
which works but may pull in system headers that aren't freestanding-safe.

## Build Instructions

### Quick Build

```bash
make all    # Build kernel, libc, userspace, initrd, ISO
make run    # Run in QEMU with 4GB RAM
```

### Build Targets

```bash
make kernel      # Build kernel only
make libc        # Build C library
make userspace   # Build user programs
make initrd      # Create initial ramdisk
make iso         # Build bootable ISO
make run         # Standard QEMU (4GB RAM)
make run-debug   # QEMU with GDB server (-s -S)
make run-kernel  # Direct kernel boot (no ISO)
```

### Build Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| DEBUG | 0, 1 | 0 | Enable debug output |
| OPTIMIZE | 0-3 | 2 | Optimization level |

```bash
make DEBUG=1 OPTIMIZE=0   # Debug build
make DEBUG=0 OPTIMIZE=3   # Release build
```

### Clean

```bash
make clean        # Remove all build artifacts
```

## Project Structure

```
LestraOS/
├── boot/              # Bootloader (multiboot2)
│   ├── boot.asm       # 32→64 bit transition
│   ├── stage1.asm     # 16-bit MBR bootloader (raw HDD only)
│   └── grub.cfg       # GRUB config for ISO
├── kernel/
│   ├── arch/          # x86_64 (GDT, IDT, ISRs, linker.ld)
│   ├── core/          # kernel_main, printk, panic, shell
│   ├── drivers/       # vga, keyboard, serial, pit
│   ├── mm/            # PMM, VMM, heap
│   ├── sched/         # Scheduler (stub)
│   ├── syscall/       # SYSCALL/SYSRET + dispatch
│   ├── fs/            # VFS (in-memory) + initrd loader
│   ├── ui/            # Cyberpunk UI (themes, panels, menus)
│   ├── ai/            # AI subsystem (NEW)
│   └── include/       # Kernel headers
├── libc/              # Custom C library
├── user/              # Userspace programs
├── pkg/               # Package manager (60+ prebuilt)
├── desktop/           # Desktop environment stub
├── installer/         # Host-side OS installer
├── build/             # Build scripts
│   ├── mkinitrd.py
│   └── cross-compiler.sh
├── docs/              # Architecture, build, boot, AI docs
└── Makefile
```

## Booting

### QEMU (recommended)

```bash
make run
# or:
qemu-system-x86_64 -cdrom build/lestraos.iso -m 4096M -cpu qemu64 \
  -vga std -serial stdio -boot d -no-reboot -no-shutdown
```

You should see:
1. GRUB menu with "Lestra OS" entry
2. VGA shows "B" in top-left (boot marker)
3. Kernel boot log on serial (`B123456L` + log messages)
4. Boot splash with ASCII art logo
5. UI menu or shell prompt

### Real hardware

To install on a USB drive or hard disk:

```bash
# Build the raw HDD image first (uses stage1.asm):
make build/lestraos.img

# Write to USB:
sudo ./installer/install.sh --target /dev/sdX --image build/lestraos.img

# Or to a file:
./installer/install.sh --target my_image.vhd --image build/lestraos.img --auto
```

### Debugging

```bash
make run-debug
# In another terminal:
gdb build/kernel.bin
(gdb) target remote :1234
(gdb) break kernel_main
(gdb) continue
```

## Troubleshooting

### "grub-mkrescue not found"
```bash
sudo apt install grub-pc-bin xorriso
```

### "nasm not found"
```bash
sudo apt install nasm
```

### "qemu-system-x86_64 not found"
```bash
sudo apt install qemu-system-x86
```

### "Cross compiler not found"
Either build it:
```bash
./build/cross-compiler.sh
export PATH=$HOME/opt/cross/bin:$PATH
```
Or just use system gcc — the Makefile will fall back automatically.

### Kernel boots then immediately reboots
This usually means a triple-fault. Run with `-no-reboot -d int` to see
the exception:
```bash
qemu-system-x86_64 -cdrom build/lestraos.iso -m 4096M \
  -serial stdio -no-reboot -d int
```

### VGA shows garbage / no output
The VGA driver writes to 0xB8000 (identity-mapped). If you see garbled
text, the page tables may not have been set up correctly. Check boot.asm
and the serial output for the `B123456L` markers.

### Page fault at 0xFFFFFFFF800xxxxx
The kernel runs identity-mapped (no higher-half alias). Any code that
references `0xFFFFFFFF80000000+` addresses will page-fault. Fix by
using the identity-mapped physical address instead.

## License

MIT License - See LICENSE file for details.
