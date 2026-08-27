# Userspace

Userspace lives at `user/` — alias `userspace` in docs (real path stays `user/` to avoid breaking Makefiles).

## Layout

- `init/` — init.c (PID 1, mounts VFS, spawns shell)
- `shell/` — shell.c (interactive shell, builtins)
- `bin/` — hello.c, sysinfo.c (example ELFs)

## Build

`make -C user` or top-level `make userspace` (requires `build/libc/libc.a` first).

ELFs are freestanding (`-nostartfiles -nostdlib -mno-sse -mno-red-zone -mcmodel=large`) — see `user/Makefile`. Windows equivalent is covered by `build-kernel.ps1` linking `libc.a` into `kernel.bin`.

See `docs/STRUCTURE.md` (userspace alias) and `docs/USERSSPACE.md` (67 syscalls 0–66).
