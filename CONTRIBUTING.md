# Contributing

## Build

Ubuntu/Debian:

```bash
sudo apt update
sudo apt install build-essential nasm qemu-system-x86 grub-pc-bin xorriso python3
make all && make run
```

Or build each stage:

```bash
make kernel      # kernel only
make libc        # C library
make userspace   # user programs
make initrd      # initrd.img
make iso         # bootable ISO
```

## Windows

```powershell
powershell -ExecutionPolicy Bypass -File "scripts/build-kernel.ps1"
# expected: 138 C + 7 asm, LINK OK
```

Or use WSL2 (Ubuntu 24.04) inside Windows — same Ubuntu steps as above (`make all && make run`).

## Style

- Real implementations only, no faking
- `clang-format` if present
- One commit per logical change, `fix:`/`feat:`/`docs:` prefix
- Keep `kernel/include/lestra/*.h` include paths stable
- `make clean` must not delete source; `build/` and `iso/` are ignored
