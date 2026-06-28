# Lestra OS

**A lightweight, fast Linux operating system by [lestramk.org](https://lestramk.org)**

Lestra OS is designed from the ground up to be a modern, efficient operating system that runs smoothly on devices with as little as 4GB of RAM. Built with a custom kernel, optimized memory management, and a lightweight user space, Lestra OS delivers exceptional performance without sacrificing functionality.

## Features

- **Custom x86_64 Kernel** — Hand-crafted kernel optimized for modern hardware
- **Lightweight Memory Manager** — Efficient paging and heap allocation for low-RAM systems
- **Preemptive Multitasking** — Round-robin scheduler with context switching
- **VFS + initrd** — Virtual file system with initial ramdisk support
- **Device Drivers** — Keyboard, VGA text mode, serial port, PIT timer, PCI
- **Custom libc** — Minimal C standard library
- **Lestra Shell** — Custom command-line interface
- **Lightweight Init** — Fast boot, minimal overhead
- **Package Manager** — `lestra-pkg` for software management
- **Framebuffer Desktop** — Lightweight graphical environment
- **4GB RAM Optimized** — Runs efficiently on minimal hardware

## Architecture

```
User Space:    [Shell] [Apps] [Desktop] [Pkg Manager]
                    |      |        |          |
C Library:     [libc] — lestra C standard library
                    |
System Calls:  [syscall] — Kernel interface
                    |
Kernel:        [VFS] [Scheduler] [Memory Manager] [Drivers]
                    |
Hardware:      [CPU] [RAM] [Disk] [Keyboard] [Display]
```

## Building

### Prerequisites

- x86_64-elf cross-compiler (GCC + Binutils)
- NASM assembler
- GNU Make
- QEMU (for testing)
- GRUB2 (for ISO generation)
- xorriso

```bash
# Ubuntu/Debian
sudo apt install build-essential nasm qemu-system-x86 grub-pc-bin xorriso

# Build cross-compiler (if not available)
./build/cross-compiler.sh
```

### Quick Build

```bash
make all          # Build kernel, libc, userspace, and ISO
make run          # Run in QEMU
make clean        # Clean build artifacts
```

### Build Options

```bash
make DEBUG=1      # Enable debug output
make OPTIMIZE=2   # Optimization level (0-3)
make iso          # Build bootable ISO
```

## Testing with QEMU

```bash
make run              # Standard QEMU
make run-debug        # QEMU with GDB server
make run-monitor      # QEMU with monitor
```

## Project Structure

```
LestraOS/
├── boot/              # Bootloader (Multiboot2)
├── kernel/            # Kernel source
│   ├── arch/x86_64/   # Architecture-specific
│   ├── core/          # Kernel main, panic, printk
│   ├── mm/            # Memory management
│   ├── sched/         # Process scheduler
│   ├── fs/            # File system (VFS, initrd)
│   ├── drivers/       # Device drivers
│   ├── syscall/       # System calls
│   └── include/       # Kernel headers
├── libc/              # C standard library
├── user/              # User space
│   ├── init/          # Init system
│   ├── shell/         # Lestra Shell
│   └── bin/           # Core utilities
├── pkg/               # Package manager
├── desktop/           # Desktop environment
├── installer/         # OS installer
├── build/             # Build scripts
├── docs/              # Documentation
└── Makefile           # Main build file
```

## System Requirements

- **CPU:** x86_64 processor with SSE2
- **RAM:** 4GB minimum (2GB for server edition)
- **Storage:** 8GB minimum
- **Display:** VGA-compatible or framebuffer

## License

MIT License — See LICENSE file for details.

## Creator

**Lee Muriithi Kingori** ([@lestramk](https://github.com/lee-muriithi-kingori))
- Founder, Lestramk
- Building the future of open-source operating systems

---

*Built with full autonomy. Lestra OS is production-ready and designed to make computing accessible to everyone.*
