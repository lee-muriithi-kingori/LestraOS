# LestraOS Structure

```
LestraOS/
├── boot/               # stage1.asm, boot.asm, grub.cfg — Multiboot2 + MBR
├── kernel/             # 40k lines C+ASM — 17 subdirs, see kernel/README.md
│   ├── arch/x86_64/    # GDT/IDT/ISR, linker.ld, framebuffer
│   ├── core/           # kernel_main, panic, printk, shell
│   ├── mm/             # PMM bitmap, VMM paging, heap
│   ├── sched/          # preemptive RR, context_switch.asm
│   ├── syscall/        # 67 calls 0-66, SYSCALL/SYSRET
│   ├── exec/           # ELF + PE, ldso, pipe, signals
│   ├── fs/             # VFS + ext2/FAT32/procfs/devfs/tmpfs/tarfs
│   ├── net/            # TCP/IP + TLS 1.2 + HTTP + wifi framework
│   ├── drivers/        # block/net/char/pci/apic/audio/clock/...
│   ├── gui/            # 42-file compositor 60Hz
│   ├── ui/             # cyberpunk text-mode UI (themes, panels)
│   ├── ai/ + audio/    # GGUF inference, TTS/STT
│   ├── acpi/           # ACPI tables & power mgmt
│   ├── pkg/            # lestra-pkg, preinstalled manifests
│   ├── sys/            # cron, sandbox, ssh_server, net_config
│   └── include/lestra/ # public headers <lestra/...> — keep stable
├── libs/libc/          # actually at libc/ — C library (keep path, document alias)
├── userspace/          # actually at user/ — init + shell + bin
├── tools/              # actually at scripts/ — mkinitrd, mkext2, cross-compiler
├── docs/               # ARCHITECTURE, BUILD, NETWORKING, etc.
├── third_party/lestramanika # GGUF submodule
├── .github/workflows/  # CI build + smoke boot
├── screenshots/        # boot/docs images
├── installer/          # host-side installer (install.sh, install.py)
├── initrd_content/     # initrd seed content (wallpaper.rgb)
└── Makefile + env.sh + LICENSE
```

> `libs/libc` and `userspace` and `tools` are *aliases* — real paths stay `libc/`, `user/`, `scripts/` this plan to avoid breaking Makefiles.
