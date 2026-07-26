# LestraOS

**A custom x86_64 operating system by [Lee Muriithi Kingori](https://github.com/lee-muriithi-kingori) — founder of [lestramk.org](https://lestramk.org)**

> Building an OS from scratch: kernel, drivers, libc, userspace, and beyond.

LestraOS is a hobbyist/research OS written in C and x86 assembly. It targets
real x86_64 hardware and boots on QEMU. The goal is to understand every layer
of the stack — from hardware interrupts to userspace syscalls.

## Status (this version)

| Component | Status |
|---|---|
| Custom x86_64 kernel (boots on QEMU) | **Working** |
| VGA text mode driver | Working |
| PS/2 keyboard driver (scancodes, IRQ, ring buffer) | Working |
| Serial port driver (COM1) | Working |
| PIT timer driver (IRQ0) | Working |
| Custom C library (libc) | Core functions |
| Custom shell (lsh) with full command set | Working |
| Memory manager (PMM bitmap + VMM paging + heap) | Working |
| Preemptive scheduler | **Working** — real round-robin scheduler, context switching, fork/exec/wait/zombies |
| VFS (in-memory) + initrd loader | Working |
| System calls (SYSCALL/SYSRET) | **Working** — real entry path, 29 syscalls dispatched |
| **Package manager (lestra-pkg) — 60+ prebuilt packages** | **NEW** |
| **AI subsystem (multi-provider, agentic tools)** | **NEW** — real HTTP client; HTTPS providers work via in-tree TLS 1.2 |
| **Cyberpunk UI (3 themes, panels, menus)** | **NEW** |
| Framebuffer desktop | **Working** — real compositor (`kernel/gui/`), wired into boot in `kernel_main.c`. Software-rendered only, no GPU accel, no USB HID |
| TCP/IP stack + TLS 1.2 (client + server) | **Working** — ARP/ICMP/UDP/DHCP, real TLS handshake (AES-GCM, ECDHE P-256, X.509, RSA) |

## What was fixed in this version

See [`docs/BOOT.md`](docs/BOOT.md) for the full list of 12 boot-blocking
bugs that were identified and fixed. The short version:

1. Kernel was linked at 0x10000 (64KB) instead of 0x100000 (1MB)
2. VGA driver used an unmapped higher-half address → page fault on first print
3. Higher-half mapping in boot.asm was broken (PDPT[510] never set)
4. Shadow `initrd_init` in kernel_main masked the real one in vfs.c
5. Wrong `USER_CS` (0x23 instead of 0x1B) → #GP on first SYSRET
6. VFS fds 0-2 collided with stdin/stdout/stderr
7. PMM `mark_region` missed the last partial page
8. Makefile cross-compiler fallback pointed to non-existent `x86_64-lestra-`
9. `ld` wasn't told to use `elf_x86_64` emulation
10. `build/mkinitrd.py` was referenced but missing
11. `build/cross-compiler.sh` was referenced but missing
12. Multiboot2 tag alignment issues in boot.asm

## Highlights

### Boot flow

```
BIOS/UEFI → GRUB2 → boot.asm (32→64 bit transition) → kernel_main()
   → GDT, IDT, PIC, PMM, VMM, heap, sched, syscall, VFS, initrd
   → timer (1000Hz), keyboard
   → pkg_init() (package manager)
   → ai_init() (AI subsystem + tool registration)
   → sti() (enable interrupts)
   → ui_boot_splash() → ui_menu_loop() or shell_run()
```

### Cyberpunk UI

Three themes accessible from the shell or UI menu:

```
lestra:/$ theme cyan    # default - cyan neon cyberpunk
lestra:/$ theme amber   # amber phosphor (retro CRT)
lestra:/$ theme green   # green phosphor (matrix-style)
```

UI features:
- ASCII art boot splash
- Boxed panels with titles
- Title bar + status bar
- System tools panel (memory, CPU, uptime, processes)
- Main menu with system tools / installer / about / theme switch / AI

### Package manager (lestra-pkg)

60+ prebuilt packages including Python, Node, GCC, Vim, Git, and more:

```
lestra:/$ pkg list                # show all 60+ packages
lestra:/$ pkg install python      # install Python 3.11
lestra:/$ pkg install node        # install Node.js 20
lestra:/$ pkg installed           # what's installed
lestra:/$ pkg search editor       # find editors
lestra:/$ pkg info gcc            # show GCC details
lestra:/$ pkg remove python       # uninstall
```

Each install simulates download progress, dependency resolution, unpacking,
and configuration. Dependencies are auto-installed (e.g., installing `gcc`
auto-installs `binutils`).

### AI subsystem

Multi-provider AI with agentic tool-calling:

```
lestra:/$ ai keys set openai sk-...
lestra:/$ ai keys set claude sk-ant-...
lestra:/$ ai keys list

lestra:/$ ai chat explain quantum computing
lestra:/$ ai agent install nginx and check memory
lestra:/$ ai tools                  # list available tools
lestra:/$ ai providers              # list supported providers
```

Tools available to the AI:
- `shell` — run shell commands
- `file_read` / `file_write` — VFS file operations
- `pkg_install` / `pkg_list` — package management
- `meminfo` — memory statistics
- `uptime` — system uptime

See [`docs/AI.md`](docs/AI.md) for the full AI documentation.

### Shell (lsh)

Built-in commands:
- **System:** `help`, `uname`, `version`, `uptime`, `free`, `meminfo`,
  `cpuinfo`, `ps`, `neofetch`, `test`, `echo`, `clear`, `reboot`, `shutdown`
- **UI:** `ui` (launch menu), `theme <cyan|amber|green>`
- **Packages:** `pkg install|remove|list|installed|search|info`
- **AI:** `ai keys|chat|agent|tools|providers`
- **Files:** `file ls|cat|write`
- **Other:** `install` (host installer help), `exit`

## Quick Start

```bash
# Prerequisites (Ubuntu/Debian)
sudo apt install build-essential nasm qemu-system-x86 grub-pc-bin xorriso

# Build cross-compiler (optional but recommended)
./build/cross-compiler.sh
export PATH=$HOME/opt/cross/bin:$PATH

# Build everything (kernel, libc, userspace, initrd, ISO)
make all

# Run in QEMU
make run

# Clean build
make clean
```

## Architecture

```
┌─────────────────────────────────────────────────────┐
│  User Space — working (fork/exec/wait, ELF loader)   │
│  [Lestra Shell] [sysinfo] [init] [bin/*]            │
├─────────────────────────────────────────────────────┤
│  libc — memcpy, memset, strlen, printf,             │
│         malloc/free, read, write, exit               │
├─────────────────────────────────────────────────────┤
│  System Calls (SYSCALL/SYSRET interface, 29 calls)   │
├─────────────────────────────────────────────────────┤
│  Kernel                                              │
│  [Print] [Panic] [GDT] [IDT] [IRQ] [VMM] [Sched]    │
│  [VGA]  [Keyboard] [Serial] [PIT Timer] [Heap]      │
│  [VFS + ext2 + initrd] [UI] [GUI compositor]         │
│  [Package Manager] [AI] [TCP/IP + TLS 1.2]          │
├─────────────────────────────────────────────────────┤
│  Hardware: CPU, RAM, Keyboard, Serial, PIT, NIC     │
└─────────────────────────────────────────────────────┘
```

## Project Structure

```
LestraOS/
├── boot/           # Multiboot2 bootloader (boot.asm + grub.cfg)
├── build/          # Build scripts
│   ├── mkinitrd.py        # Initrd image builder
│   └── cross-compiler.sh  # x86_64-elf toolchain builder
├── kernel/
│   ├── arch/       # x86_64 arch (GDT, IDT, ISRs, linker.ld)
│   ├── core/       # kernel_main, panic, printk, shell
│   ├── drivers/    # vga, keyboard, serial, pit
│   ├── mm/         # PMM, VMM, heap
│   ├── sched/      # Real preemptive round-robin scheduler + context switch
│   ├── syscall/    # SYSCALL/SYSRET + dispatch (29 syscalls)
│   ├── fs/         # VFS + ext2 driver + procfs + devfs + initrd loader
│   ├── net/        # TCP/IP stack: ARP, ICMP, UDP, DHCP, DNS, TCP, TLS 1.2 client+server
│   ├── gui/        # Framebuffer compositor (widgets, top bar, app grid,
│   │                 file explorer, terminal, task manager) — wired into
│   │                 boot via kernel_main.c
│   ├── ui/         # Cyberpunk text-mode UI (themes, panels, menus)
│   ├── ai/         # AI subsystem (providers, tools, chat)  ← NEW
│   └── include/    # Kernel headers
├── libc/           # Custom C library
├── user/           # Userspace programs (init, shell, sysinfo)
├── pkg/            # Package manager (lestra-pkg)  ← ENHANCED
├── desktop/        # Dead code — not called anywhere; kernel_main.c calls
│                     kernel/gui/ directly instead. Delete or repurpose.
├── installer/      # OS installer (host-side)
├── docs/           # Architecture, build, boot, AI docs
└── Makefile
```

## Building

See [`docs/BUILD.md`](docs/BUILD.md) for detailed build instructions.

```bash
make all          # Build everything
make kernel       # Kernel only
make iso          # Bootable ISO
make run          # Run in QEMU
make run-debug    # QEMU + GDB server
make clean        # Clean all
```

## Documentation

- [`docs/BOOT.md`](docs/BOOT.md) — What was broken and how it's fixed (20 bugs)
- [`docs/CHANGES.md`](docs/CHANGES.md) — Per-file diff summary
- [`docs/BUILD.md`](docs/BUILD.md) — Build instructions
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — Kernel architecture
- [`docs/AI.md`](docs/AI.md) — AI subsystem and agentic tools

## Roadmap

- [x] TCP/IP stack (ARP, ICMP, UDP, DHCP) — hand-rolled, not lwIP
- [x] TLS 1.2 client + server (AES-GCM, ECDHE P-256, X.509, RSA) — see `kernel/net/tls.c`
- [x] Real preemptive scheduler with context switching — see `kernel/sched/`
- [x] ext2 filesystem driver — see `kernel/fs/ext2/`
- [x] Userspace process loading (ELF loader) — see `kernel/exec/elf.c`
- [x] DNS resolver — see `net_resolve()` in `kernel/net/net.c` (real UDP query + A-record parsing)
- [ ] TLS 1.3 (currently 1.2 only)
- [ ] POSIX-compatible libc
- [ ] Real package execution — downloads work and land on disk, but there's no ELF package runtime yet to unpack/exec them (see `kernel/pkg/lestra-pkg.c` for the honest breakdown of what "install" currently does)
- [ ] WiFi driver (ath9k/rtl) — currently simulated, no real hardware support
- [ ] USB host controller driver

## Contributing

Issues and PRs welcome. Areas where help is especially useful:

- TCP/IP stack integration (for AI subsystem)
- Memory management (paging, heap allocation)
- Preemptive multitasking / round-robin scheduler
- VFS with ext2 / initrd support
- POSIX-compatible libc
- Framebuffer graphics / desktop environment

## License

MIT License — see [LICENSE](./LICENSE)

## Contact

**Lee Muriithi Kingori** — [@lestramk](https://github.com/lestramk-org) — [lestramk.org](https://lestramk.org)

*LestraOS is an independent project. Built for the love of the stack.*
