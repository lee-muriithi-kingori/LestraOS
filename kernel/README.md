# Kernel

LestraOS kernel — 40k lines C + x86_64 ASM. Public headers: `kernel/include/lestra/*.h`, include path is `<lestra/...>`.

## Layout

- `arch/x86_64/` — GDT/IDT/ISR, linker.ld, framebuffer, boot entry
- `core/` — kernel_main, panic, printk, shell, userspace_boot
- `mm/` — PMM bitmap, VMM paging, heap, page_fault
- `sched/` — preemptive round-robin, context_switch.asm
- `syscall/` — 67 syscalls 0–66, SYSCALL/SYSRET entry
- `exec/` — ELF + PE, ldso, pipe, signals, futex, TLS
- `fs/` — VFS + ext2 (`fs/ext2/`), FAT32, procfs, devfs, tmpfs, tarfs
- `net/` — TCP/IP, TLS 1.2, HTTP, socket, wifi framework
- `drivers/` — block (ahci/nvme/virtio_blk), net (e1000/rtl8139/rtl8168/virtio_net), char (keyboard/mouse/pty/serial/timer/vga), pci, apic (lapic/ioapic), audio (ac97), clock (hpet/rtc), power (battery), sensor (temp)
- `gui/` — 42-file compositor, 60 Hz, dock/top_bar/window manager
- `ui/` — cyberpunk text-mode UI (themes, panels)
- `ai/` + `audio/` — pickle GGUF inference, offline, TTS/STT
- `acpi/` — ACPI tables and power management
- `pkg/` — lestra-pkg, deb, preinstalled manifests
- `sys/` — cron, device_id, sandbox, ssh_server, net_config
- `include/lestra/` — public kernel headers (`<lestra/...>`) — keep stable

## Build

Built via top-level `Makefile` (`make all`) and `powershell -ExecutionPolicy Bypass -File "scripts/build-kernel.ps1"` — 138 C + 7 asm, LINK OK.
Headers are included as `#include <lestra/foo.h>` → `kernel/include/lestra/foo.h`.

## Notes

See `docs/STRUCTURE.md` and `docs/ARCHITECTURE.md` for the big picture.
