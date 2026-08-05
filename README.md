# <picture><img src="docs/assets/lestraos-banner.png" alt="LestraOS" width="720"></picture>

<div align="center">

# ⚡ LestraOS

### A custom x86_64 operating system — built from the silicon up.

`kernel · drivers · libc · userspace · networking · TLS · AI · cloud`

**by [Lee Muriithi Kingori](https://github.com/lee-muriithi-kingori)** · founder of [lestramk.org](https://lestramk.org)

<br>

<table>
<tr>
<td align="center">🖥️ <b>Architecture</b><br><sub>x86_64, long mode</sub></td>
<td align="center">🧬 <b>Boot</b><br><sub>GRUB2 / Multiboot2 / raw MBR</sub></td>
<td align="center">🛡️ <b>License</b><br><sub>MIT</sub></td>
<td align="center">🔄 <b>CI Loop</b><br><sub>Autonomous, 15-min cadence</sub></td>
</tr>
</table>

<br>

[![Boot Status](https://img.shields.io/badge/boot-QEMU%20%E2%9C%93-brightgreen)](./logs/boot-cloud-mode-after-fix.log)
[![Kernel](https://img.shields.io/badge/kernel-x86__64-blue)](./kernel)
[![Branch](https://img.shields.io/badge/branch-main-protected-red)](./)
[![Discussions](https://img.shields.io/badge/discuss-lestraOS%20Lounge-9cf)](https://github.com/lee-muriithi-kingori/LestraOS/discussions/13)

</div>

---

> *Building an OS from scratch: kernel, drivers, libc, userspace, networking, crypto, AI — and now, a cloud/VPS mode that boots headless and serves SSH + HTTP over a real E1000 NIC. Every layer of the stack, understood.*

## 🎯 What is LestraOS?

LestraOS is a hobbyist/research operating system written in C and x86 assembly. It targets real x86_64 hardware and boots on QEMU. The goal is radical: **understand every layer of the stack** — from hardware interrupts to userspace syscalls, from a hand-rolled TCP/IP stack to a self-signed TLS 1.2 server, from a preemptive scheduler to an in-kernel AI subsystem.

This is not a toy. It's a working OS.

## 🚀 Quick Start

```bash
# Prerequisites (Ubuntu/Debian)
sudo apt install build-essential nasm qemu-system-x86 grub-pc-bin xorriso

# Build everything (kernel, libc, userspace, initrd, ISO)
make all

# Boot in QEMU (GUI desktop mode)
make run

# Or boot cloud/VPS mode (headless, serial console, SSH + HTTP)
make run-cloud    # see Makefile target

# Clean
make clean
```

> **No sudo?** This repo's CI boots on a toolchain installed entirely in `~/.local` (NASM, QEMU, grub-mkrescue, xorriso all extracted from .debs without root). See [`docs/BUILD.md`](docs/BUILD.md).

## 📸 Boot Proof

| Cloud/VPS boot (serial console) |
|---|
| ![LestraOS cloud boot](./screenshots/boot-cloud-mode-fixed.png) |

Full serial log: [`logs/boot-cloud-mode-after-fix.log`](./logs/boot-cloud-mode-after-fix.log)

The kernel initializes: GDT, IDT, PIC, PMM/VMM, heap, scheduler, syscalls, VFS + initrd, PIT timer, keyboard, package manager (110 packages), AI subsystem (7 tools), E1000 NIC + DHCP + IPv6, firewall, RTC, power management, thermal sensors, WiFi framework, cron daemon, service manager, sandbox subsystem — **then enters cloud mode, starts the SSH server, and acquires an IP via DHCP.** No crash. 🟢

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
│  [GDT][IDT][ISR][IRQ][APIC][PIT] [PMM][VMM][Heap] [Sched]     │
│  [VGA][Kbd][Serial][E1000][AC97][RTC][ACPI]             │
│  [VFS][ext2][initrd][procfs][devfs]                     │
│  [CSPRNG][TLS 1.2][P-256 ECDH][AES-GCM][X.509][RSA]     │
│  [SSH-2.0 server][HTTP mgmt API][compositor][UI themes] │
│  [Package manager][AI agentic tools][sandbox][cron]      │
├─────────────────────────────────────────────────────────┤
│  Hardware: CPU, RAM, Keyboard, Serial, PIT, NIC, RTC     │
└─────────────────────────────────────────────────────────┘
```

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
│   ├── ai/           # Multi-provider AI with agentic tools
│   └── include/      # Kernel headers
├── libc/           # Custom C library
├── user/           # Userspace (init, shell, sysinfo, bin/*)
├── installer/      # Host-side installer
├── docs/           # Architecture, build, boot, AI, networking docs
├── scripts/        # mkinitrd, mkext2, cross-compiler
├── screenshots/    # Boot proof screenshots
├── logs/           # Captured boot logs
└── Makefile
```

## 🛣️ Roadmap

- [x] Custom x86_64 kernel boots on QEMU
- [x] GDT, IDT, PIC, PIT, PMM, VMM, heap
- [x] Preemptive scheduler with context switching + fork/exec/wait
- [x] SYSCALL/SYSRET with 29+ syscalls
- [x] VFS + ext2 + initrd + procfs + devfs
- [x] TCP/IP stack (ARP, ICMP, UDP, DHCP, DNS, TCP) — hand-rolled, not lwIP
- [x] TLS 1.2 client + server (AES-GCM, ECDHE P-256, X.509, RSA)
- [x] SSH-2.0 remote shell server (port 2222)
- [x] HTTP/HTTPS management API (/status, /metrics, /reboot, /shutdown)
- [x] Cloud/VPS headless boot mode (serial console)
- [x] Package manager (110 packages, 5 repos)
- [x] AI subsystem (7 agentic tools, multi-provider)
- [x] CSPRNG with RDRAND + TSC fallback (CPUID-gated — see changelog)
- [x] Cyberpunk UI (3 themes: cyan, amber, green)
- [x] Framebuffer compositor
- [ ] Interrupt-mixed entropy pool (currently TSC-only on RDRAND-less VMs — INSECURE)
- [ ] TLS 1.3
- [ ] POSIX-compatible libc
- [ ] Real package ELF execution runtime
- [ ] USB host controller driver
- [ ] WiFi driver (ath9k/rtl) — currently framework-only

## 🤝 Contributing

Issues and PRs welcome. The `main` branch is **protected**:
- Pull requests require **1 approving review** before merge
- **Linear history** enforced (rebase, no merge commits)
- **No force pushes**, **no deletions** of `main`
- Outside contributors: your PR will be reviewed by the maintainer before merge — no auto-merge

Areas where help is especially useful:
- TCP/IP stack hardening
- Memory management (paging, heap coalescing)
- POSIX libc compatibility
- Framebuffer / GPU acceleration
- Real hardware drivers (USB, WiFi)

## 💬 Community

👉 **[lestraOS Lounge](https://github.com/lee-muriithi-kingori/LestraOS/discussions/13)** — the place to talk.

Ideas, questions, bug reports that don't fit an issue, roadmap thoughts, cool experiments — all welcome. Be excellent to each other. Push hard, ship bold.

## 📜 Changelog

> Dated, brief, honest. Newest first.

- **5 Aug 2026** — ⚡ **KE-23: enable Local APIC + IOAPIC interrupt controller.** Replaced the legacy 8259 PIC with LAPIC+IOAPIC as the system interrupt controller. New `kernel/drivers/apic/` subsystem: `lapic.c` (xAPIC enable via IA32_APIC_BASE MSR, spurious vector 0xFF, LVT mask, EOI, IPI support), `ioapic.c` (24-entry redirection table, GSI routing with polarity/trigger from ACPI MADT IntSrcOverride), `apic.c` (orchestrator with PIC fallback). Updated `irq.c`: `register_irq_handler`/`irq_enable`/`irq_disable`/`pic_send_eoi` transparently switch between PIC and IOAPIC backends. ISA IRQs routed through IOAPIC using ACPI MADT mappings. Fixed missing `idt_reload()` after installing spurious vector gate (would #GP on first spurious APIC interrupt). Removed duplicate dead `acpi_init()` call. Unlocks MSI/MSI-X, USB XHCI, LAPIC timer, SMP. Boot-verified 2x: LAPIC id=0 at 0xFEE00000, IOAPIC 24 entries at 0xFEC00000, DHCP 10.0.2.15, all 7 security features active, zero faults.
- **5 Aug 2026** — 🐛 **KE-22: UI bug squash + git hygiene + sti;hlt hang fix + ACPI table discovery + overlay wiring.** Fixed top bar slide-in animation inversion, editor Enter-key data loss, editor backspace-merge buffer overflow, task_sleep() hang (bare hlt with IF=0). Wired 8 dead overlay subsystems into compositor (right-click context menu, volume/brightness popups, keyboard shortcuts). Wired EV_MOUSE_SCROLL into terminal (256-line scrollback), editor, file_explorer. New kernel/acpi/ subsystem: RSDP discovery, RSDT/XSDT walking, FACP parser (SCI=9, PM1a_CNT=0x604), MADT parser (LAPIC=0xfee00000, IOAPIC=0xfec00000, 5 ISA overrides), HPET parser (base=0xfed00000, 2 comparators). acpi_isa_irq_to_gsi() API. Unlocks IOAPIC setup, HPET timer, USB XHCI interrupt routing. Fixed top bar slide-in animation inversion (bar was sliding OFF screen instead of ON — `bar_y = TB_MARGIN_TOP - TB_HEIGHT + tb_visible` now correctly starts above screen and slides down). Fixed editor.c Enter-key data loss bug (tail after cursor was dropped instead of moved to new line — now properly splits the line via `memcpy`). Fixed editor.c backspace-merge buffer overflow (`line_lens[prev_row]` was set to `prev_len + cur_len` with no clamp — now clamped to `MAX_LINE_LEN-1`, overflow chars dropped). Fixed `task_sleep()` hang: used bare `hlt()` but SYSCALL clears RFLAGS.IF (SFMASK=0x200), so `hlt` with IF=0 halts forever — replaced with `sti; hlt` idiom (sti has a one-instruction delay before interrupts are taken, eliminating the race). Wired 8 dead overlay subsystems into the compositor: lock_screen, power_menu, screenshot, clipboard, shortcuts, context_menu, brightness, volume_slider — all were fully implemented but never rendered or dispatched events. Added right-click desktop context menu, Ctrl+Alt+L (lock), Ctrl+Alt+P (power), Ctrl+Alt+B (brightness) shortcuts. Wired `EV_MOUSE_SCROLL` into terminal (256-line scrollback ring buffer), editor (scroll_row), file_explorer (list_scroll + scrollbar thumb). Git hygiene: stopped tracking 151 files (132 build .o files, iso/ outputs, logs/, qemu-data symlink, MEMORY.md/AUDIT_FIXES.md/WIRING_NOTES.md dev notes, scripts/smoke_cloud.sh + fix_smap_compat.py that leaked /home/z/ paths). Updated .gitignore to prevent re-tracking. Boot-verified: all 7 security features active, Intellimouse scroll wheel detected, zero faults.
- **5 Aug 2026** — 🔐 **KE-21: SMAP-harden signals + wire Linux compat signals + futex + task_sleep.** Replaced all bare user-pointer dereferences in `signals.c` with SMAP-safe `access_ok` + `copy_from_user`/`copy_to_user` wrappers — the signal subsystem was the last major handler without SMAP protection (on real hardware with CR4.SMAP=1, any `sigaction()` call would triple-fault). Wired Linux compat layer (`LINUX_SYS_RT_SIGACTION/PROCMASK/RETURN/KILL`) to forward to the native signal implementation (were no-ops returning 0). Connected the orphaned `futex_dispatch()` (136-line hash-table + wait-queue implementation) to `sys_futex`, replacing the no-op stub — `FUTEX_WAIT` now actually blocks via `task_block()` and `FUTEX_WAKE` unblocks waiters. SMAP-fixed `futex.c` (replaced bare `*uaddr` with `get_user()`). Implemented real `task_sleep()` with timer-based wake deadlines via `wake_tick` field + `sched_check_wakeups()` — `poll()`/`select()` callers no longer busy-loop at 100% CPU. Fixed `task_block()` stuck-in-BLOCKED bug for single-process callers. GUI scroll wheel wired into compositor dispatch, editor, terminal, and file explorer.
- **5 Aug 2026** — 🖱️ **KE-20: PS/2 mouse Intellimouse scroll wheel + middle button fix + /dev/mouse.** Added Intellimouse extension detection via magic sample rate sequence (200/100/80) — QEMU's PS/2 mouse responds with ID=0x03, enabling 4-byte packets with scroll wheel delta (signed byte 3). Added `EV_MOUSE_SCROLL` input event type routed through the input subsystem. Fixed missing middle button events (MOUSE_BTN_MIDDLE press/release was never generated). Added `/dev/mouse` and `/dev/input/mouse0` device nodes in devfs (reads `struct mouse_event` via non-blocking `read()`). Added `scroll` field to `mouse_event` and input event structs. Boot-verified: `mouse: Intellimouse detected (ID=0x03, scroll wheel enabled)`, 4-byte packets, all 7 security features active, zero faults.
- **5 Aug 2026** — 🔧 **KE-19: fix virtio_blk I/O timeout + FAT32 end-to-end.** Root cause: GCC -O2 hoisted `vblk_used->idx` read out of the polling loop — the virtqueue used ring is in regular BSS RAM (DMA-written by device), not MMIO, so the compiler cached the value in a register and the driver never observed request completion. Fix: cast to `volatile struct virtq_used*` inside the poll loop to force real memory loads. Also fixed legacy transport queue size clamping (device computes layout from its own QueueNum; clamping causes avail/used ring offset mismatch). FAT32 read-only filesystem now works end-to-end on QEMU 10.0.11: virtio-blk modern transport, BPB parsing, root directory listing, file reads via FAT chain walking. Verified: 3 files read from 16MB test image, all 7 security features active, zero faults.
- **5 Aug 2026** — 🧠 **KE-17: sys_mmap returns real VMAs instead of kmalloc pointers.** Replaced the `kmalloc()`-based mmap stub with proper VMA-backed allocations. `sys_mmap` now allocates user virtual addresses in a dedicated mmap region (0x60000000+, PDPT[1]) via `vmm_map_page(cur->pml4, ...)` with 8-bit ASLR slide on first call. Physical pages are zeroed before mapping (SMAP-safe — avoids kernel-mode dereference of user VAs). OOM rollback frees all already-mapped pages. `sys_munmap` now actually frees physical pages and unmaps PTEs (was a no-op leak). Honors `PROT_WRITE` (RW vs RO) and `PROT_EXEC` (NX bit). Foundation for dynamic linking, JIT, shared memory.
- **5 Aug 2026** — 🔐 **KE-16: interrupt-mixed entropy pool.** Fixed broken partial KE-16 work (duplicate variable declarations in keyboard.c/mouse.c, broken `if()` in timer.c, drain bug in entropy.c that only wrote 21 of 48 bytes, csprng.c XOR into uninitialized buffer). Implemented 16-slot lock-free XOR accumulator (`entropy.h` + `entropy.c`) fed by timer IRQ0 (TSC jitter between fires), keyboard IRQ1 (scancode timing), and mouse IRQ12 (packet timing). Each IRQ handler does ~25 cycles: one `rdtsc` + one XOR + one indexed store. Pool is drained on CSPRNG reseed via `entropy_drain()` which folds 16 uint64_t slots into 48 bytes with bit-shift diffusion. Added RDSEED (NIST SP 800-90B) as secondary hardware entropy source, CPUID-gated (no-op on qemu64). Security audit now shows `Entropy pool: ACTIVE`. Boot-verified on `qemu64,+smep,+smap` with E1000 DHCP.
- **5 Aug 2026** — 🛡️ **KE-15: kernel heap KASLR (KASLR-lite).** Randomized the kernel heap base address at boot using early TSC-based entropy (runs before CSPRNG init). The heap start varies within a 192 MB window (64–256 MB range, 2 MB aligned), providing 8 bits of entropy. Every `kmalloc()` return address is different on each boot. Changed `KERNEL_HEAP_START`/`KERNEL_HEAP_END` from compile-time macros to runtime variables set by `heap_init()`. Security audit now shows `KASLR-lite: ENABLED`. Boot-verified: three consecutive boots produced heap addresses at 0x6c00000, 0x4000000, and 0xc80000.
- **5 Aug 2026** — 🔌 **KE-14: NIC driver abstraction (struct nic_ops vtable).** Eliminated the `active_nic` integer switch (0=e1000, 1=virtio, 2=rtl8139) and 15 `extern` function declarations in `net.c`. Created `include/lestra/nic.h` with `struct nic_ops` vtable (init, send, recv, get_mac, flush). Created `net/nic.c` with driver priority table. Each driver (e1000, virtio_net, rtl8139) now exports a `const struct nic_ops` instance. Migrated E1000 and VirtIO-net from duplicated raw PCI scan loops to the shared `pci_find_device()` / `pci_get_device()` API (RTL8139 already used it). `net_init()` iterates the vtable array; `net_tick()` dispatches through function pointers. Adding a new NIC driver now requires only implementing `nic_ops` and adding one line to the table. Boot-verified on both E1000 and VirtIO-net QEMU configs.
- **5 Aug 2026** — 🔧 **KE-13: fix GP fault in user_map_page during ELF loading.** Root cause: `create_user_address_space()` shared `boot_pml4[0..3]` by pointer, giving the user PML4 direct references to boot 2MB huge page PD entries. `user_map_page()` encountered `PAGE_HUGE` at the PD level, misinterpreted the huge page physical address as a PT pointer, and wrote PTEs to arbitrary physical memory — corrupting kernel BSS/data and causing a delayed #GP. Fix: deep-copy boot page tables (PDPT+PD levels) into private per-process copies via `deep_copy_pdpt()` + `deep_copy_pd()`. When `user_map_page()` encounters a 2MB huge page in the user's private PD copy, `split_2mb_huge_page()` converts it to 512 4KB PTEs (with `PAGE_USER` cleared for kernel memory). Kernel boot page tables are never modified. Boot-verified on `qemu64,+smep,+smap`: `/init` loads 4 PT_LOAD segments and jumps to userspace with zero faults.
- **5 Aug 2026** — 🔌 **KE-12: RTL8139 NAPI-style deferred CAPR update + TX cleanup.** Removed per-packet CAPR write that caused QEMU `can_receive()` deadlock (QEMU model uses `RxBufPtr + 16` offset internally; writing CAPR forward reduces available space). Added `rtl8139_recv_flush()` hook in `net_tick()` (no-op CAPR write, placeholder for future IRQ-driven RX). Removed ISR clear after TX to avoid QEMU model side effects. Added TX timeout diagnostic. E1000 regression verified. RTL8139 first-packet RX confirmed; QEMU model multi-packet quirk documented (buffer wraps at 8K, not 64K, per `rtl8139_reset_rxring`).
- **5 Aug 2026** — 🔌 **KE-11: RTL8139 RX fixes.** Fixed 4 critical bugs: (1) RCR register bit definitions all shifted by 1 position (AB=0x08→0x04, APM=0x02→0x10, etc.) — RX was silently rejecting all packets. (2) RX/TX DMA buffers allocated via kmalloc() from 0x10000000+ heap region (outside guest physical RAM) — hardware DMA writes silently dropped. Fixed by using static BSS arrays (matching E1000 pattern). (3) RX status header struct had swapped field order (len before flags instead of flags before len). (4) CAPR written as absolute address instead of RBSTART offset. First-packet RX now works (DHCP OFFER received). E1000 regression: passed. Created `kernel/drivers/net/rtl8139.c` (~300 LOC) — first non-virtio, real-hardware NIC driver. Uses IO-port access (BAR0 = IO base, `inb`/`outb`/`inw`/`outl`), unlike E1000 (MMIO). Discovered via KE-9's `pci_find_device(0x10EC, 0x8139)`. 4-entry TX ring with TSAD/TSD registers and OWN-bit polling. 64 KB RX ring buffer with wrap-around packet copy (Beta-mandated edge case). Wired into `net.c` as 3rd driver candidate (priority: virtio > e1000 > rtl8139). Refactored `net.c` from `use_virtio_net` int to `active_nic` enum (0=e1000, 1=virtio, 2=rtl8139). Boot-verified: E1000 regression passed (DHCP 10.0.2.15), RTL8139 detected at PCI 00:03.0, IO base 0xC000, TX sends DHCP DISCOVER. lestraOS now supports 3 NIC drivers: VirtIO-net, E1000, RTL8139.
- **5 Aug 2026** — 🔌 **KE-9: shared PCI bus enumeration + lspci.** Created `kernel/drivers/pci/pci.c` + `kernel/include/lestra/pci.h` — shared PCI config space access API (`pci_config_read8/16/32`, `pci_config_write32`), `pci_scan_bus()` device table, `pci_find_device()`/`pci_find_class()` lookup helpers. Added `lspci` shell command (lists all PCI devices with BDF, vendor:device, class, IRQ, BAR). Boot discovers 5 devices: Intel host bridge, PIIX4 PM, IDE, ACPI, QEMU VGA. Migrated all 7 drivers (e1000, virtio_net, ahci, virtio_blk, ac97, ac97_capture, battery) from duplicated local PCI code to the shared API — 211 lines of duplication deleted. Unblocks RTL8139 and other future drivers via clean `pci_find_device(vendor, device)`.
- **5 Aug 2026** — 🛡️ **KE-8: TIER 2c execve/ldso argv/envp hardening.** Rewrote `sys_execve` to copy argv/envp from user space into kernel buffers via new `copy_argvec_from_user()` helper before passing to ldso. Fixed argc=0 bug (was `argv ? (int)0 : 0`). Fixed auxv/envp stack overlap in `ldso_load_and_run` (auxv at `sp[3+argc]` overwrote `envp[1]`). Stack layout now follows proper Linux ABI: argc, argv[], NULL, envp[], NULL, auxv[]. Added `strnlen_user()` and `copy_argvec_from_user()` to `uaccess.h` with `LESTRA_ARG_MAX`/`LESTRA_ARG_BYTES_MAX` caps. Boot-verified on `qemu64,+smep,+smap` — SMEP/SMAP/ASLR all active, zero faults.
- **5 Aug 2025** — 🛡️ **KE-7: last SMAP violations eliminated.** Fixed all remaining direct user-pointer dereferences: `sys_bind`/`sys_connect` now copy `sockaddr` from user via `copy_from_user` bounce buffer before passing to socket layer. `sys_accept` fills a kernel-local `sockaddr_in`, then copies back to user via `copy_to_user`/`put_user`. In `linux_compat.c`: `LINUX_SYS_UNAME`, `LINUX_SYS_GETTIMEOFDAY`, `LINUX_SYS_NANOSLEEP`, `LINUX_SYS_SYSINFO` all rewritten to build structs in kernel stack buffers and `copy_to_user`/`copy_from_user` instead of writing directly to user memory. `LINUX_SYS_FSTAT` uses `clear_user` instead of direct `memset`. Every syscall path in both native and Linux-compat dispatchers is now SMAP-clean. Boot-verified on `qemu64,+smep,+smap` with zero SMAP faults.
- **5 Aug 2025** — 🎲 **TIER 5: minimal ASLR enabled.** Randomized USER_STACK_TOP by 12 bits (16 MB range, page-aligned) using `csprng_u64()`. Randomized initial `brk` by 8 bits (1 MB range). `sys_mmap` ASLR deferred (kmalloc stub needs vmm_map_page). Consolidated 3 duplicate `USER_STACK_TOP` definitions (elf.c, ldso.c, page_fault.c) into `USER_STACK_TOP_DEFAULT` in `mm.h`. `/proc/security` and SECURITY AUDIT now show `ASLR: ENABLED`. Boot-verified with all 5 protections active simultaneously: SMEP + SMAP + NX + ASLR + Stack Canaries.
- **5 Aug 2025** — 🛡️ **TIER 3: SMEP/SMAP ENABLED + PTE_PHYS_MASK fix.** Flipped CR4.SMEP (bit 20) and CR4.SMAP (bit 21) in `gdt_init()` after `ltr_load()`. Boot-verified on `qemu64,+smep,+smap` — kernel runs full cloud init (SSH, DHCP, TLS) under SMAP with zero faults. Fixed a critical PTE address mask bug: `~0xFFFULL` preserves bit 63 (PAGE_NX), producing non-canonical addresses when NX is set. Root cause was `elf.c:user_map_page` applying PAGE_NX to intermediate page table entries (PML4/PDPT/PD). Fixed by (1) adding `PTE_PHYS_MASK` (bits 51:12) to `mm.h` and replacing ALL `~0xFFFUL` extractions across `elf.c`, `vmm.c`, `page_fault.c`, `scheduler.c` (28 sites), and (2) stripping NX from intermediate table flags in `user_map_page`. The GUI boot path (`elf_exec /init`) no longer crashes with #GP.
- **5 Aug 2025** — 🔒 **TIER 2b bounce buffers + SMAP diagnostic.** Converted all remaining user-pointer-exposing syscalls to SMAP-safe patterns: `sys_read`/`sys_write` now use kernel bounce buffers (kmalloc 4 KB, copy_from_user/copy_to_user) so VFS/pipe/socket layers never see user pointers. `sys_getdents` copies dirent structs one-at-a-time via stack-local variable. `sys_send`/`sys_recv` use bounce buffers. `sys_poll` copies the entire pollfd array into kmalloc, processes in kernel, writes revents back via `put_user`. `sys_select` copies fd_set arrays into stack-local buffers. `sys_ioctl` FIONREAD uses `put_user`. Added SMAP violation diagnostic to `page_fault.c` — when CR4.SMAP is enabled, a supervisor-mode #PF on a user address prints a clear "POSSIBLE SMAP VIOLATION" message with the offending RIP. Smoke test passes on both `qemu64` and `qemu64,+smep,+smap`. All ~48 syscalls are now SMAP-safe (except `sys_execve`+ldso.c, deferred to TIER 2c). TIER 3 (CR4.SMEP/SMAP flip) is now unblocked.
- **5 Aug 2025** — 🔒 **TIER 2 syscall user-pointer wrappers landed.** Converted 14 syscall entry points in `kernel/syscall/syscall.c` to use `copy_from_user`/`copy_to_user`/`strncpy_from_user`/`get_user`/`put_user` from `kernel/include/lestra/uaccess.h` (previously unused). Hardened syscalls: `open`, `execve`, `getcwd`, `chdir`, `mkdir`, `rmdir`, `stat`, `unlink`, `uname`, `pipe`, `chmod`, `fstat`, `access`, `rename`, `waitpid`, `times`, `clock_gettime`, `getrlimit`, `setrlimit`, `futex`. Added `nfds` caps on `poll`/`select` (`LESTRA_POLL_MAX=1024`) to prevent kernel-stack exhaustion via huge fd_set arrays. Added `get_user`/`put_user` single-word accessors + security cap constants (`LESTRA_ARG_MAX`, `LESTRA_ARG_BYTES_MAX`, `LESTRA_PATH_MAX`) to `uaccess.h`. Smoke test passes on both `qemu64` and `qemu64,+smep,+smap` CPUs (CR4 bits still OFF — TIER 3 flip deferred per BETA-3 caution). Updated `scripts/smoke_cloud.sh` to auto-detect the rootless dev toolchain at `/home/z/.local/opt/devtools`. Boot logs: `logs/boot-tier2-uaccess-wrappers.log`, `logs/boot-tier2-smep-smap-cpu.log`.
- **4 Aug 2025** — 🔥 **First successful cloud-mode boot in CI.** Fixed `#UD` (Invalid Opcode) crash at `collect_entropy()` by CPUID-gating `rdrand`/`rdseed` in `kernel/include/lestra/types.h`. Without the gate, QEMU's `qemu64` CPU (no RDRAND) crashed on the first SSH host-key generation. Now boots fully: SSH server starts, CSPRNG initializes (TSC fallback, labelled INSECURE), DHCP acquires `10.0.2.15`. Setup: NASM 2.16 + QEMU 10.0.11 + grub-mkrescue 2.12 installed rootless in `~/.local`. Branch protection on `main` enabled. Discussions enabled (Lounge thread live).
- **4 Aug 2025** — Bootstrapped autonomous dev loop: 15-min cron job armed (job `307143`) to continuously improve, fix, and push lestraOS. Multi-agent deliberation model deployed (High-Reward strategist ALPHA + Caution officer BETA + HR-deployed specialist engineers).
- **Prior commits** — see `git log`. Highlights: ELF loading fixes, VMM page-walk fix, keyboard 0xE0 handling, syscall reboot, NX bit, swapgs, COW fork, page-fault handler, per-process FD tables, ext2→VFS plumbing, real CPU temp sensor, ACPI shutdown, PS/2 mouse, WAV/PCM codec, AI agentic loop, compositor z-order focus, SSH-2.0, VirtIO drivers, PTY multiplexing, WPA2 handshake, IPv6, TLS server.

## 📄 License

MIT License — see [LICENSE](./LICENSE).

## 👤 Contact

**Lee Muriithi Kingori** — [@lestramk-org](https://github.com/lestramk-org) — [lestramk.org](https://lestramk.org)

<div align="center">

---

*LestraOS is an independent project.*  
*Built for the love of the stack.*  
*⚡ lestramk — push hard, ship bold.*

</div>
