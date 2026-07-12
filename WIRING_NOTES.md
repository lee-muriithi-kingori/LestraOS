# LestraOS — Wiring Summary (v2: Networking + AI + UI)

This file documents every fix and feature added to LestraOS. The original
source had 6 critical bugs that prevented booting; v1 fixed those. v2 adds
a real TCP/IP stack, HTTP client, package downloads, and a pre-built AI
client.

## Quick start

```bash
# 1. Build (Debian/Ubuntu: apt install nasm gcc qemu-system-x86 xorriso grub-pc-bin)
make all

# 2. Boot in QEMU with networking
make run
# or: make GRUB_MODULES_DIR=/usr/lib/grub/i386-pc run

# 3. In the shell (type '0' to skip the boot menu):
network                 # show IP, MAC, gateway, DNS
ping 10.0.2.2           # ICMP ping the QEMU gateway
wget http://example.com/  # HTTP GET
ai providers            # show configured AI providers
ai setkey glm YOUR_KEY  # set GLM API key
ai setmodel glm glm-5.2 # use GLM 5.2
ai chat Hello!          # chat with the AI (needs HTTP endpoint, see NETWORKING.md)
pkg install hello       # download a demo package via HTTP
help                    # see all commands
```

## What's new in v2

### UI — branded with author attribution

- Boot banner now shows "by Lee Muriihi Kingori" and "lestramk.org (c) 2026"
- Boot splash title bar: "Lestra OS - by Lee Muriihi Kingori"
- Author attribution line below the ASCII art logo
- About panel updated with networking + AI feature list
- Shell welcome: "Lestra Shell (lsh) 1.0 - by Lee Muriihi Kingori"
- Status panel shows: Network (DHCP/HTTP), AI (GLM 5.2 / Claude compatible)

### Networking — full TCP/IP stack

New files:
- `kernel/include/lestra/net.h` — public API
- `kernel/drivers/net/e1000.c` — Intel 82540EM NIC driver (PCI + MMIO + DMA)
- `kernel/net/net.c` — Ethernet + ARP + IP + ICMP + UDP + DHCP + DNS
- `kernel/net/tcp.c` — TCP state machine (one-shot, request/response)
- `kernel/net/http.c` — HTTP/1.0 client (GET + POST)

Boot.asm now identity-maps **4 GB** instead of 1 GB (needed for PCI MMIO
at 0xFEBxxxxx). Uses 4 PDs × 512 huge 2MB pages = 2048 entries.

Verified working in QEMU:
- E1000 PCI scan finds NIC at 00:3.0, MMIO 0xfebc0000
- MAC read from EEPROM: a4:a8:0:24:68:ac
- DHCP DISCOVER → OFFER → REQUEST → ACK: IP 10.0.2.15, gw 10.0.2.2, DNS 10.0.2.3
- ICMP ping to 10.0.2.2: reply in 0-1 ms
- ARP resolves gateway MAC
- TCP SYN goes out with correct IP + TCP checksums

### Package downloads — real HTTP

`pkg/lestra-pkg.c` updated:
- `struct package` has a new `url` field
- 4 demo packages with real HTTP URLs: `hello`, `rfc2616`, `rfc791`, `rfc1035`
- `pkg install <name>` does a real HTTP GET when the package has a URL and
  the network is up, then stores the bytes in VFS at `/var/packages/<name>`
- Falls back to simulated progress when no URL or no network

### AI client — pre-built, works with GLM 5.2 / Claude / Ollama

`kernel/ai/ai.c` updated:
- `ai_http_post()` replaced: was simulated, now makes a real HTTP POST with
  an OpenAI-compatible JSON body (`{"model":"...","messages":[...]}`)
- Parses the `"content":"..."` field from the response JSON
- Pre-configured for 4 providers: openai, claude, gemini, glm
- Default GLM model: glm-4.6 (override with `ai setmodel glm glm-5.2`)
- New shell commands:
  - `ai setkey <provider> <key>` — set API key
  - `ai setendpoint <provider> <url>` — override endpoint URL
  - `ai setmodel <provider> <model>` — override model name
  - `ai providers` — show all configured providers + endpoints

### Serial input for shell interaction

`kernel/drivers/char/keyboard.c` updated: `keyboard_getchar()` and
`keyboard_has_key()` now fall back to reading from COM1 (serial) when the
PS/2 keyboard buffer is empty. This lets you interact with the shell via
QEMU's `-nographic` mode (which pipes stdin to the serial port).

### In-kernel snprintf

`kernel/core/printk.c` has a new `ksnprintf()` function — minimal but
correct. Supports `%s %d %u %x %c %%` with `l`/`ll`/`z` length modifiers.
Used by the HTTP client and AI client to build request strings.

## v1 fixes (still in place)

### Compile blockers
1. `kernel_main.c` — `struct mb2_module* mod` declared after use
2. `pkg/lestra-pkg.c` — `extern void hlt(void)` conflicted with `hlt` macro
3. `libc/src/unistd.c` — `syscall()` was variadic with no asm input constraints
4. `user/bin/sysinfo.c` — 32-bit `pushfl` in long mode + `=b` constraint under PIC
5. `user/init/init.c` — `extern shell_run` referenced a separate binary
6. `user/bin/sysinfo.c` + `user/init/init.c` — no `_start` symbol

### Boot blockers
7. `kernel/arch/x86_64/gdt.c` — TSS descriptor array too small + wrong base encoding
8. `kernel/include/lestra/mm.h` — heap at 0x40000000 was past the 1GB identity map
9. `boot/boot.asm` — multiboot2 info-request tag size=24 but had 5 entries
10. `boot/boot.asm` — `mb_info_ptr` saved before BSS zeroing (got wiped)
11. `kernel/mm/pmm.c` — bitmap sized to all mmap entries (1 TB), clobbered GRUB data
12. `Makefile` — `grub-mkrescue` invoked without `-d`, producing non-bootable ISO

### Wiring
13. `Makefile` — relative `-I` paths broke libc sub-make (fixed with `$(CURDIR)`)
14. `make clean` deleted source scripts (moved `build/` → `scripts/`)
15. `libc/src/stdio.c` + `stdlib.c` — old variadic `syscall()` externs conflicted
16. `boot/stage1.asm` — jump target 0x11E00 should be 0x10E00
17. `kernel/include/lestra/gdt.h` — dangling `gdt_reload` declaration removed

## v2 networking fixes (in addition)

18. **IP/TCP/ICMP checksum byte order** — `inet_checksum()` reads bytes as
    big-endian 16-bit words (correct for network checksum), but storing the
    result as a `uint16_t` on little-endian x86_64 swaps the bytes. Fixed by
    wrapping all checksum assignments in `htons16()`.

19. **DHCP message type hardcoded** — `dhcp_send()` always wrote option 53
    with value 1 (DISCOVER) regardless of the `msg_type` parameter. Fixed
    to use the parameter.

20. **DHCP packet size check** — `dhcp_handle()` rejected packets smaller
    than `sizeof(struct dhcp_pkt)` (552 bytes), but server replies are
    often shorter. Fixed to accept any packet ≥ 240 bytes (fixed header
    + magic cookie).

21. **net_tick() re-entrancy** — called from timer IRQ AND from synchronous
    wait loops (tcp_connect, arp_resolve, etc.). Without a guard, an IRQ
    during a wait loop would re-enter and corrupt static state. Added
    `in_net_tick` flag.

## Known limitations (intentional)

- **No HTTPS/TLS.** Cloud AI APIs are HTTPS-only. Use a local TLS-terminating
  proxy (socat/nginx) or a local LLM (Ollama). See `docs/NETWORKING.md`.
- **One TCP connection at a time.** Sufficient for shell request/response.
- **No TCP retransmit timer.** LAN use is fine; WAN packet loss will stall.
- **Scheduler is still a stub** (single-task mode).
- **Most syscalls return -1** (read/write/getpid work).
- **VFS is in-memory only** (no persistent storage).

## File structure

```
LestraOS/
├── boot/               # boot.asm (multiboot2 + 4GB identity map), stage1.asm, grub.cfg
├── kernel/
│   ├── arch/x86_64/    # GDT, IDT, ISR, linker.ld
│   ├── core/           # kernel_main, printk (+ksnprintf), panic, shell
│   ├── drivers/
│   │   ├── char/       # VGA, keyboard (+serial fallback), serial, timer (+net_tick)
│   │   └── net/        # e1000.c (NEW)
│   ├── mm/             # PMM, VMM, heap
│   ├── sched/          # scheduler stub
│   ├── syscall/        # SYSCALL/SYSRET + dispatch
│   ├── fs/             # VFS + initrd loader
│   ├── net/            # net.c, tcp.c, http.c (ALL NEW)
│   ├── ui/             # cyberpunk UI (boot splash, menu, themes)
│   ├── ai/             # AI client (real HTTP POST, 4 providers)
│   └── include/lestra/ # all headers (net.h is NEW)
├── libc/               # freestanding C library
├── user/               # init, shell, sysinfo
├── pkg/                # package manager (65 catalog entries, 4 with real URLs)
├── desktop/            # desktop stub
├── installer/          # host-side install scripts
├── scripts/            # mkinitrd.py, cross-compiler.sh (moved from build/)
├── docs/               # ARCHITECTURE, BOOT, BUILD, NETWORKING (NEW), AI, CHANGES
└── Makefile
```
