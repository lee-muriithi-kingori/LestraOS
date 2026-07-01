# LestraOS

**A custom x86_64 operating system by [Lee Muriithi Kingori](https://github.com/lee-muriithi-kingori) — founder of [lestramk.org](https://lestramk.org)**

> Building an OS from scratch: kernel, drivers, libc, userspace, and beyond.

LestraOS is a hobbyist/research OS written in C and x86 assembly. It targets real x86_64 hardware and boots on QEMU. The goal is to understand every layer of the stack — from hardware interrupts to userspace syscalls.

## Status

| Component | Status |
|---|---|
| Custom x86_64 kernel | Working |
| VGA text mode driver | Working |
| PS/2 keyboard driver (scancodes, IRQ, ring buffer) | Working |
| Serial port driver (COM1) | Working |
| PIT timer driver (IRQ0) | Working |
| Custom C library (libc) | Core functions |
| Custom shell (Lestra Shell) | Basic REPL |
| Memory manager | Planned |
| Preemptive scheduler | Planned |
| VFS + initrd | Planned |
| System calls | Planned |
| Package manager | Planned |
| Framebuffer desktop | Planned |

Currently at **~2,200 lines** of hand-written kernel and driver code. The foundation is solid; the subsystems are being built.

## Highlights

- **Keyboard driver** — full PS/2 scancode set 1→2 translation, IRQ handler with ring buffer, overflow protection
- **Serial driver** — COM1 debug output, works with `screen` / `minicom`
- **Timer driver** — PIT in square wave mode, configurable frequency, IRQ0
- **Custom libc** — `memcpy`, `memset`, `strlen`, `printf`-family, `malloc`/`free`
- **Custom shell** — REPL with internal commands, built on the kernel's serial I/O
- **Multiboot2 bootloader** — GRUB-compatible, passes kernel size and memory map

## Quick Start

```bash
# Prerequisites (Ubuntu/Debian)
sudo apt install build-essential nasm qemu-system-x86 grub-pc-bin xorriso

# Build cross-compiler if needed
./build/cross-compiler.sh

# Build everything
make all

# Run in QEMU
make run

# Clean build
make clean
```

## Architecture

```
┌─────────────────────────────────────────────┐
│  User Space                                │
│  [Lestra Shell] [sysinfo] [init] [bin/*]  │
├─────────────────────────────────────────────┤
│  libc — memcpy, memset, strlen, printf,    │
│         malloc/free, read, write, exit      │
├─────────────────────────────────────────────┤
│  System Calls (kernel interface)            │
├─────────────────────────────────────────────┤
│  Kernel                                     │
│  [Print] [Panic] [GDT] [IDT] [IRQ]        │
│  [VGA]  [Keyboard] [Serial] [PIT Timer]    │
├─────────────────────────────────────────────┤
│  Hardware: CPU, RAM, Keyboard, Serial, PIT │
└─────────────────────────────────────────────┘
```

## Project Structure

```
LestraOS/
├── boot/          # Multiboot2 bootloader stage
├── kernel/        # Kernel source
│   ├── arch/     # x86_64 arch code (GDT, IDT, ISRs)
│   ├── core/     # kernel_main, panic, printk
│   ├── drivers/  # vga, keyboard, serial, pit
│   └── include/  # kernel headers
├── libc/         # Custom C standard library
├── user/         # Userspace programs
│   ├── shell/    # Lestra Shell
│   └── bin/      # sysinfo, init, etc.
├── build/        # Build scripts, cross-compiler
├── docs/         # Architecture notes
└── Makefile      # Main build
```

## Contributing

Issues and PRs welcome. Areas where help is especially useful:

- Memory management (paging, heap allocation)
- Preemptive multitasking / round-robin scheduler
- VFS with ext2 / initrd support
- POSIX-compatible libc
- Package manager (`lestra-pkg`)
- Framebuffer graphics / desktop environment

## License

MIT License — see [LICENSE](./LICENSE)

## Contact

**Lee Muriithi Kingori** — [@lestramk](https://github.com/lestramk-org) — [lestramk.org](https://lestramk.org)

*LestraOS is an independent project. Built for the love of the stack.*
