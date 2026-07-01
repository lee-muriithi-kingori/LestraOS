# Building Lestra OS

## Prerequisites

### Required Tools

| Tool | Version | Purpose |
|------|---------|---------|
| x86_64-elf-gcc | 13.x | Cross compiler |
| nasm | 2.16+ | Assembler |
| make | 4.x | Build system |
| qemu-system-x86_64 | 8.x | Emulator |
| grub-mkrescue | 2.12 | ISO creation |
| xorriso | 1.5+ | ISO tool |

### Ubuntu/Debian

```bash
sudo apt update
sudo apt install build-essential nasm qemu-system-x86 grub-pc-bin xorriso
```

### Build Cross Compiler

```bash
./build/cross-compiler.sh
export PATH=$HOME/opt/cross/bin:$PATH
```

## Build Instructions

### Quick Build

```bash
make all    # Build everything
make run    # Run in QEMU with 4GB RAM
```

### Build Targets

```bash
make kernel      # Build kernel only
make libc        # Build C library
make userspace   # Build user programs
make initrd      # Create initial ramdisk
make iso         # Build bootable ISO
```

### Run Options

```bash
make run          # Standard QEMU (4GB RAM)
make run-debug    # QEMU with GDB server
make run-kernel   # Direct kernel boot (no ISO)
```

### Clean

```bash
make clean        # Remove all build artifacts
```

## Build Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| DEBUG | 0, 1 | 0 | Enable debug output |
| OPTIMIZE | 0-3 | 2 | Optimization level |

```bash
make DEBUG=1 OPTIMIZE=0   # Debug build
make DEBUG=0 OPTIMIZE=3   # Release build
```

## Project Structure

```
LestraOS/
├── boot/          # Bootloader (multiboot2)
├── kernel/        # Kernel source
│   ├── arch/      # Architecture-specific (x86_64)
│   ├── core/      # Main, printk, panic
│   ├── mm/        # Memory management
│   ├── sched/     # Process scheduler
│   ├── fs/        # File system
│   ├── drivers/   # Device drivers
│   └── include/   # Kernel headers
├── libc/          # C standard library
├── user/          # User space programs
├── pkg/           # Package manager
├── desktop/       # Desktop environment
├── installer/     # OS installer
├── build/         # Build scripts
└── docs/          # Documentation
```

## Troubleshooting

### QEMU not found
```bash
sudo apt install qemu-system-x86
```

### GRUB not found
```bash
sudo apt install grub-pc-bin
```

### Cross compiler not found
```bash
export PATH=$HOME/opt/cross/bin:$PATH
```

## License

MIT License - See LICENSE file for details.
