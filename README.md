# ⚡ LestraOS

### A custom x86_64 operating system — built from the silicon up.

`kernel · drivers · libc · userspace · networking · TLS · AI · cloud`

**by [Lee Muriithi Kingori](https://github.com/lee-muriithi-kingori)** · founder of [lestramk.org](https://lestramk.org)

| | |
|---|---|
| 🖥️ **Architecture** | x86_64, long mode |
| 🧬 **Boot** | GRUB2 / Multiboot2 / raw MBR |
| 🛡️ **License** | MIT |
| 🔄 **CI** | QEMU boot smoke test |
| 📦 **Packages** | 110 (across 5 repos) |
| 🧠 **AI** | 7 agentic tools + in-kernel `pickle` GGUF selftest |

---

![Meet the Yugi family — LestraOS on real hardware](./screenshots/photo_2026-08-20_09-01-59.jpg)

*Meet the Yugi family — LestraOS booting on real hardware.*

---

> *Building an OS from scratch: kernel, drivers, libc, userspace, networking, crypto, AI — and a cloud/VPS mode that boots headless and serves SSH + HTTP over a real E1000 NIC. Every layer of the stack, understood.*

## 🎯 What is LestraOS?

LestraOS is a hobbyist/research operating system written in C and x86 assembly. It targets real x86_64 hardware and boots on QEMU. The goal is radical: **understand every layer of the stack** — from hardware interrupts to userspace syscalls, from a hand-rolled TCP/IP stack to a self-signed TLS 1.2 server, from a preemptive scheduler to an in-kernel AI subsystem.

This is not a toy. It's a working OS — **boots, initializes every subsystem, gets a DHCP lease, and drops to an interactive shell.**

## 📸 Boot Proof (real screenshots)

These are actual QEMU screenshots captured from a fresh `make all && make run` — not mockups.

### Cloud/VPS mode (serial console)

Real QEMU serial output. The kernel initializes GDT, IDT, APIC, PMM/VMM, heap, scheduler, syscalls, VFS + initrd, VFS selftest (8/8 PASS), PIT, keyboard, package manager (110 packages), AI subsystem (7 tools + pickle GGUF selftest PASSED), PCI scan, E1000 NIC + DHCP (IP 10.0.2.15), firewall, IPv6, RTC, power management, thermal sensors, WiFi framework, cron, service manager, sandbox, security audit (NX/ASLR/canaries enabled), then enters cloud mode and starts the SSH server.

![LestraOS cloud boot](./screenshots/boot-cloud-mode.png)

### GUI mode (framebuffer compositor)

Real QEMU framebuffer screenshot (1024×768). The kernel initializes the VESA framebuffer, starts the compositor with the cyberpunk-cyan UI theme, and renders the splash animation + app grid + top bar.

![LestraOS GUI boot](./screenshots/boot-gui-mode.png)

### Legacy text shell (interactive)

Real QEMU serial output with interactive commands. Shows the `help` output, `sysinfo` (uptime, memory, network, IPv6, battery, CPU temp, cron), `netstat` (IP/gateway/DNS/MAC/IPv6/WiFi/firewall), `services` (4 services: net/shell/ssh/sandbox-server), `whoami`, and `hostname`.

![LestraOS legacy shell](./screenshots/boot-legacy-shell.png)

Full serial logs: [`logs/`](./logs/) (generated at boot time, not checked in)

## 🚀 Quick Start

```bash
# Prerequisites (Ubuntu/Debian)
sudo apt install build-essential nasm qemu-system-x86 grub-pc-bin xorriso

# Clone with the lestramanika submodule (in-kernel AI engine)
git clone --recurse-submodules https://github.com/lee-muriithi-kingori/LestraOS.git
cd LestraOS
# Already cloned? initialise the submodule:
git submodule update --init --recursive

# Build everything (kernel, libc, userspace, initrd, ISO)
make all

# Boot in QEMU (GUI desktop mode)
make run

# Or boot cloud/VPS mode (headless, serial console, SSH + HTTP)
make run-cloud    # see Makefile target

# Clean
make clean
```

> **No sudo?** This repo's CI boots on a toolchain installed entirely in `~/.local` (NASM, QEMU, grub-mkrescue, xorriso, SeaBIOS, iPXE ROMs all extracted from .debs without root). See [`docs/BUILD.md`](docs/BUILD.md) and `env.sh`.

> **Submodules:** this repo vendors [`lestramanika`](https://github.com/lee-muriithi-kingori/lestramanika) — the from-scratch GGUF inference engine that powers the in-kernel AI selftest — at `third_party/lestramanika`. The kernel-compatible core is synced into `kernel/ai/` via `./scripts/sync_lestramanika.sh`. See [`docs/LESTRAMANIKA.md`](docs/LESTRAMANIKA.md) for the full integration guide.

## 🧱 Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Cloud/VPS Mode  —  SSH :2222 · HTTP :8080 · HTTPS:8443 │
│  Serial console (COM1) · headless · DHCP · firewall     │
├─────────────────────────────────────────────────────────┤
│  User Space — fork/exec/wait, ELF loader, lsh shell      │
│  [init] [shell] [sysinfo] [bin/*]                        │
├─────────────────────────────────────────────────────────┤
│  libc — memcpy, memset, strlen, printf, malloc/free,     │
│         read, write, exit, open/close                    │
├─────────────────────────────────────────────────────────┤
│  System Calls — SYSCALL/SYSRET, 29+ calls dispatched     │
├─────────────────────────────────────────────────────────┤
│  Kernel                                                  │
│  [GDT][IDT][ISR][IRQ][APIC][PIT] [PMM][VMM][Heap] [Sched]│
│  [VGA][Kbd][Serial][E1000][AC97][RTC][ACPI]             │
│  [VFS][ext2][initrd][procfs][devfs]                     │
│  [CSPRNG][TLS 1.2][P-256 ECDH][AES-GCM][X.509][RSA]     │
│  [SSH-2.0 server][HTTP mgmt API][compositor][UI themes] │
│  [Package manager][AI agentic tools][pickle GGUF selftest]│
│  [sandbox][cron][firewall]                               │
├─────────────────────────────────────────────────────────┤
│  Hardware: CPU, RAM, Keyboard, Serial, PIT, NIC, RTC     │
└─────────────────────────────────────────────────────────┘
```

## 🖥️ Shell Commands (verified working)

The interactive shell (`lsh`) supports these commands — verified by booting in QEMU and running each one:

| Category | Commands |
|---|---|
| **System** | `help`, `sysinfo`, `neofetch`, `uname`, `version`, `uptime`, `whoami`, `hostname`, `exit` |
| **Process** | `ps`, `free`, `cpuinfo`, `meminfo`, `reboot`, `shutdown` |
| **Network** | `netstat`, `ifconfig`, `network`, `ping`, `ping6`, `wget`, `firewall` |
| **Files** | `file` (ls/mkdir/cat/cp/mv/rmdir/rm/chmod/stat), `mount`, `save`, `exec` |
| **Hardware** | `lspci`, `battery`, `temp`, `wifi`, `disk` |
| **Services** | `services` (alias: `lee status`), `lee` (service manager + sandbox), `cron` |
| **Packages** | `packages` (alias: `pkg list`), `pkg` (install/list/search) |
| **AI** | `ai`, `claude`, `glm`, `gemini`, `openai`, `uai` |
| **UI** | `ui`, `theme`, `clear`, `date`, `time`, `play`, `speak` |

### New commands (this release)

| Command | What it does |
|---|---|
| `netstat` | Concise network status — IP, MAC, gateway, DNS, IPv6, WiFi, firewall pointer |
| `services` | Alias for `lee status` — lists all 4 registered services with state + description |
| `packages` | Alias for `pkg list` — lists 110 packages across 5 repos |
| `whoami` | Prints `root` (LestraOS is single-user) |
| `hostname` | Prints `lestraos` |

## 📦 Project Structure

```
LestraOS/
├── boot/           # Multiboot2 bootloader (boot.asm, stage1.asm, grub.cfg)
├── kernel/
│   ├── arch/x86_64/  # GDT, IDT, ISRs, TSS, linker.ld
│   ├── core/         # kernel_main, panic, printk, shell
│   ├── drivers/      # vga, keyboard, serial, pit, e1000, ac97, rtc, acpi
│   ├── mm/           # PMM (bitmap) + VMM (paging) + heap
│   ├── sched/        # Real preemptive round-robin + context switch
│   ├── syscall/      # SYSCALL/SYSRET + dispatch
│   ├── fs/           # VFS + ext2 + procfs + devfs + initrd
│   ├── net/          # ARP, ICMP, UDP, DHCP, DNS, TCP, TLS 1.2, CSPRNG, P-256
│   ├── sys/          # SSH-2.0 server, service manager
│   ├── gui/          # Framebuffer compositor (widgets, app grid, terminal)
│   ├── ui/           # Cyberpunk text-mode UI (3 themes)
│   ├── ai/           # Multi-provider AI + in-kernel pickle GGUF selftest
│   └── include/      # Kernel headers (incl. lestra/pickle.h)
├── libc/           # Custom C library
├── user/           # Userspace (init, shell, sysinfo, bin/*)
├── third_party/    # Submodules — lestramanika (GGUF inference engine)
├── installer/      # Host-side installer
├── docs/           # Architecture, build, boot, AI, networking, LESTRAMANIKA.md
├── scripts/        # mkinitrd, mkext2, cross-compiler, sync_lestramanika.sh
├── screenshots/    # Real boot screenshots (3 modes)
├── logs/           # Captured boot logs (generated at boot, gitignored)
└── Makefile
```

## 🛣️ Roadmap

- [x] Custom x86_64 kernel boots on QEMU ✅ (verified — see screenshots)
- [x] GDT, IDT, PIC, PIT, PMM, VMM, heap ✅
- [x] Preemptive scheduler with context switching + fork/exec/wait ✅
- [x] SYSCALL/SYSRET with 29+ syscalls ✅
- [x] VFS + ext2 + initrd + procfs + devfs ✅ (VFS selftest 8/8 PASS)
- [x] TCP/IP stack (ARP, ICMP, UDP, DHCP, DNS, TCP) — hand-rolled, not lwIP ✅ (DHCP lease verified)
- [x] TLS 1.2 client + server (AES-GCM, ECDHE P-256, X.509, RSA)
- [x] SSH-2.0 remote shell server (port 2222) — service registered, starts in cloud mode
- [x] HTTP/HTTPS management API (/status, /metrics, /reboot, /shutdown)
- [x] Cloud/VPS headless boot mode (serial console) ✅ (verified — see screenshot)
- [x] Package manager (110 packages, 5 repos) ✅ (verified — `packages` command)
- [x] AI subsystem (7 agentic tools, multi-provider) + in-kernel `pickle` GGUF selftest ✅ (selftest PASSED — token 5)
- [x] CSPRNG with RDRAND + TSC fallback (CPUID-gated)
- [x] Cyberpunk UI (3 themes: cyan, amber, green) ✅ (GUI screenshot shows cyan theme)
- [x] Framebuffer compositor ✅ (verified — GUI screenshot)
- [x] `netstat`, `services`, `packages`, `whoami`, `hostname` shell commands ✅ (new)
- [x] printk format-specifier fix (now handles `-`, `#`, `0`, ` ` flags) ✅ (new)
- [ ] Interrupt-mixed entropy pool (currently TSC-only on RDRAND-less VMs — INSECURE)
- [ ] TLS 1.3
- [ ] POSIX-compatible libc
- [ ] Real package ELF execution runtime
- [ ] USB host controller driver
- [ ] WiFi driver (ath9k/rtl) — currently framework-only

## 🧠 AI / lestramanika

LestraOS ships a **from-scratch, in-kernel GGUF inference engine** called [`pickle`](https://github.com/lee-muriithi-kingori/lestramanika) — **no llama.cpp, no ollama, no ggml**. It is a separate repository ([lestramanika](https://github.com/lee-muriithi-kingori/lestramanika)) tracked here as a git submodule at `third_party/lestramanika`.

### What runs in the kernel

At boot, the kernel calls `pickle_selftest()` from `kernel/ai/ai.c`. This parses an embedded 4 KB demo GGUF (`kernel/ai/pickle_demo_gguf.c`) and runs one Llama-family forward pass — RMSNorm → GQA attention with RoPE → SwiGLU FFN → output projection → argmax — entirely through an **integer-only software math library** (no FP unit needed). The boot log confirms: `pickle: selftest OK, next token = 5` and `AI: pickle self-test PASSED — forward pass produced token 5`.

See [`docs/LESTRAMANIKA.md`](docs/LESTRAMANIKA.md) for the full integration guide.

## 📜 License

MIT — see [LICENSE](./LICENSE).

## 🙏 Acknowledgements

Built by [Lee Muriithi Kingori](https://github.com/lee-muriithi-kingori). The OS is a learning project — every line is written from scratch (no vendored kernel code). The [lestramanika](https://github.com/lee-muriithi-kingori/lestramanika) submodule is also from-scratch.
