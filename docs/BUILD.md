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

### Windows

```powershell
powershell -ExecutionPolicy Bypass -File "C:\Users\leeki\AppData\Local\Temp\opencode\build-kernel.ps1"
```

This verifies `138 C + 7 asm, LINK OK` without WSL.

### Build Cross Compiler (recommended)

The cross-compiler ensures the kernel is built without any system libc
dependencies. To build it:

```bash
./scripts/cross-compiler.sh
export PATH=$HOME/opt/cross/bin:$PATH
```

This downloads and builds binutils 2.42 + gcc 13.2.0 targeted at
`x86_64-elf`. Installation goes to `~/opt/cross/`.

If you skip this step, the Makefile falls back to the system `gcc`,
which works fine on Debian/Ubuntu (the build has been verified with
system gcc 14.2).

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
make iso         # Build bootable ISO (GRUB El Torito)
make img         # Build raw HDD/USB image (stage1 MBR, no GRUB)
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
│   ├── sched/         # Real preemptive round-robin scheduler + context switch
│   ├── syscall/        # SYSCALL/SYSRET + dispatch (67 syscalls 0-66)
│   ├── fs/            # VFS + ext2 driver + procfs + devfs + initrd loader
│   ├── net/           # TCP/IP: ARP, ICMP, UDP, DHCP, DNS, TCP, TLS 1.2 client+server
│   ├── gui/           # Framebuffer compositor — wired into boot via kernel_main.c
│   ├── ui/            # Cyberpunk text-mode UI (themes, panels, menus)
│   ├── ai/            # AI subsystem (NEW)
│   └── include/       # Kernel headers
├── libc/              # Custom C library
├── user/              # Userspace programs
├── pkg/               # Package manager (60+ prebuilt)
├── desktop/           # Dead code — not called; kernel_main.c uses kernel/gui/ directly
├── installer/         # Host-side OS installer
├── scripts/           # Build scripts (was build/, moved to avoid make clean collision)
│   ├── mkinitrd.py    # packs user binaries into initrd.img
│   └── cross-compiler.sh  # builds x86_64-elf-gcc to ~/opt/cross
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
make img
# produces build/lestraos.img

# Write to USB:
sudo ./installer/install.sh --target /dev/sdX --image build/lestraos.img

# Or to a file:
./installer/install.sh --target my_image.vhd --image build/lestraos.img --auto
```

> Note: the stage1 raw-disk boot path is less tested than the GRUB ISO path. If you have trouble booting from USB, prefer the ISO (`build/lestraos.iso`) burned to a CD or written to USB with `dd if=build/lestraos.iso of=/dev/sdX bs=4M conv=notrunc`.

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
./scripts/cross-compiler.sh
export PATH=$HOME/opt/cross/bin:$PATH
```
Or just use system gcc — the Makefile will fall back automatically.

### "Boot failed: Could not read from CDROM (code 0004)"
`grub-mkrescue` couldn't find its i386-pc modules and produced a non-bootable ISO. Install `grub-pc-bin` and point the Makefile at the modules dir:
```bash
sudo apt install grub-pc-bin
make iso GRUB_MODULES_DIR=/usr/lib/grub/i386-pc
```

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
