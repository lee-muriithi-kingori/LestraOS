# libc

C library lives at `libc/` — alias `libs/libc` in docs (real path stays `libc/` to avoid breaking Makefiles).

## Build

`make -C libc` → `build/libc/libc.a` (also via `powershell -ExecutionPolicy Bypass -File "scripts/build-kernel.ps1"`).

## Contents

- `include/` — errno.h, fcntl.h, signal.h, socket.h, stat.h, stdarg.h, stdbool.h, stddef.h, stdint.h, stdio.h, stdlib.h, string.h, time.h, unistd.h
- `src/` — string.c, stdio.c, stdlib.c, unistd.c, errno.c (→ `build/libc/*.o` → `libc.a`)

Linked into `kernel.bin` via top-level `Makefile` and `scripts/build-kernel.ps1`. Headers used as `#include <string.h>` etc. via `-Ilibc/include`.

See `docs/STRUCTURE.md` (libs/libc alias) and `docs/BUILD.md`.
