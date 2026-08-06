# lestramanika — integration into lestraOS

> **lestramanika** is the standalone, from-scratch GGUF model loader and
> Llama-family inference engine that powers lestraOS's in-kernel AI
> selftest. It is a **separate repository** —
> [github.com/lee-muriithi-kingori/lestramanika](https://github.com/lee-muriithi-kingori/lestramanika)
> — written in pure freestanding C, with **no dependency on llama.cpp,
> ollama, ggml, or any other inference framework**.

This document describes how lestraOS consumes lestramanika: what is
vendored, what is not, how to sync, and how the dual-build story works.

---

## 1. The split: kernel core vs host fast path

lestramanika is built out of two layers that share the same GGUF parser
and model handle:

| Layer | Files | Runs in kernel? | Runs on host? |
|---|---|---|---|
| **Freestanding core** (soft-float, `-mno-sse` safe) | `pickle.c`, `pickle_softfp.c`, `pickle.h`, `pickle_demo_gguf.c` | ✅ yes | ✅ yes |
| **Host fast path** (native float, AVX-512 VNNI, `mmap`, OpenMP, BPE) | `pickle_fast.c`, `pickle_fast.h`, `pickle_tokenizer.c`, `pickle_host.c`, `pickle_cli.c` | ❌ no | ✅ yes |

lestraOS only consumes the **freestanding core**. The host fast path
lives exclusively in the lestramanika repo and is built there with
`make` — it requires POSIX (`mmap`, `FILE*`), SSE/AVX (the kernel is
built with `-mno-sse`), and OpenMP, none of which exist in the lestraOS
kernel.

The same `pickle.c` / `pickle_softfp.c` / `pickle.h` source compiles
both ways thanks to a single toggle:

- **Kernel build** — the lestraOS top-level `Makefile` passes
  `-DPICKLE_KERNEL`. The `#ifdef PICKLE_KERNEL` blocks at the top of
  each file pull in `<lestra/types.h>`, `<lestra/printk.h>`,
  `<lestra/mm.h>`, `<lestra/pickle.h>`, and route `kmalloc`/`kfree` to
  the kernel bump allocator. All math goes through the integer-only
  `sfp_t` soft-float layer — never C `float` arithmetic — so it links
  into a `-mno-sse` / no-x87 build.
- **Host build** — lestramanika's `Makefile` passes `-UPICKLE_KERNEL`
  (the default). Those same `#ifdef` blocks pull in `<stdint.h>`,
  `<stdio.h>`, `<stdlib.h>`, `<string.h>` instead, and
  `pickle_forward()` dispatches to the AVX-512 fast path in
  `pickle_fast.c`.

No glue or `#ifdef _HOST_` is needed inside the core files — the
`PICKLE_KERNEL` toggle handles everything, including the `pickle.h`
include path:

```c
#ifdef PICKLE_KERNEL
#include <lestra/pickle.h>   /* kernel build: -Ikernel/include   */
#else
#include "pickle.h"          /* host build:    -Isrc              */
#endif
```

This means the sync from submodule → kernel tree is a **plain verbatim
copy** — no `sed` rewriting of include paths.

---

## 2. The submodule

lestraOS tracks lestramanika as a git submodule:

```
third_party/lestramanika   →  github.com/lee-muriithi-kingori/lestramanika.git
```

`.gitmodules`:

```ini
[submodule "third_party/lestramanika"]
    path = third_party/lestramanika
    url = https://github.com/lee-muriethi-kingori/lestramanika.git
```

After a fresh clone of lestraOS, initialise the submodule:

```sh
git clone https://github.com/lee-muriithi-kingori/LestraOS.git
cd LestraOS
git submodule update --init --recursive
```

The submodule is pinned to a specific lestramanika commit (recorded in
the lestraOS tree as a gitlink). To pick up a new lestramanika release:

```sh
cd third_party/lestramanika
git fetch origin
git checkout <desired-lestramanika-commit>
cd ../..
git add third_party/lestramanika
./scripts/sync_lestramanika.sh        # vendor the new core into kernel/ai
make clean && make all                # rebuild kernel
git commit -m "ai: bump lestramanika to <commit>"
```

---

## 3. The sync script

[`scripts/sync_lestramanika.sh`](../scripts/sync_lestramanika.sh) vendors
the kernel-compatible core from the submodule into the lestraOS tree:

```
third_party/lestramanika/src/pickle.c            ->  kernel/ai/pickle.c
third_party/lestramanika/src/pickle_softfp.c     ->  kernel/ai/pickle_softfp.c
third_party/lestramanika/src/pickle_demo_gguf.c  ->  kernel/ai/pickle_demo_gguf.c
third_party/lestramanika/src/pickle.h            ->  kernel/include/lestra/pickle.h
```

Host-only files (`pickle_fast.*`, `pickle_tokenizer.c`, `pickle_host.c`,
`pickle_cli.c`) are **not** copied — they would not compile in the
kernel (`-mno-sse`, no `mmap`, no OpenMP) and are not needed there.

### Usage

```sh
./scripts/sync_lestramanika.sh            # copy core sources into kernel/ai
./scripts/sync_lestramanika.sh --check    # exit 1 if kernel has drifted from submodule
```

`--check` is suitable for CI: it prints `OK` and exits 0 when the
vendored sources match the submodule HEAD, or prints `DRIFT` lines and
exits 1 when they don't.

---

## 4. What the kernel actually uses

The lestraOS kernel calls exactly **one** pickle entry point, from
`kernel/ai/ai.c`:

```c
extern int pickle_selftest(int32_t* out_token);
...
int rc = pickle_selftest(&tok);
```

This runs at boot, parses the embedded 4 KB demo GGUF
(`pickle_demo_gguf[]` in `kernel/ai/pickle_demo_gguf.c`), executes one
Llama forward pass through the soft-float path (RMSNorm → GQA+RoPE →
SwiGLU → output projection → argmax), and prints:

```
pickle: selftest arch=llama L=1 H=2 HK=1 D=4 HD=8 VS=8
pickle: selftest OK, next token = 6
```

to the kernel console. This shipped as **KE-28** (commit `8d3300c`).
The full pickle API (metadata-only load, on-demand dequant, tensor
info, etc.) is available in `kernel/include/lestra/pickle.h` for future
in-kernel use once the kernel has a filesystem that can serve a real
GGUF file.

---

## 5. Building the host fast path

The host fast path (AVX-512 VNNI matmul, `mmap` zero-copy loader, BPE
tokenizer, OpenMP) is **not** built inside lestraOS — it is built in
the lestramanika repo directly. From the submodule checkout:

```sh
cd third_party/lestramanika
make                              # builds ./pickle (CLI) and ./pickle_selftest
./pickle selftest                 # embedded selftest → "next token = 5"
./pickle info /path/to/model.gguf
./pickle infer /path/to/model.gguf "hello" 20
./pickle chat  /path/to/model.gguf
./pickle bench /path/to/model.gguf "prompt" 32
```

On a 2-core AVX-512 VNNI host with TinyLlama-1.1B Q4_K_M (640 MB) this
delivers ~11.5 tok/s decode (49% faster than the v0.3 baseline), with
instant `<0.1 s` startup via the `mmap` zero-copy loader. See the
lestramanika [README](https://github.com/lee-muriithi-kingori/lestramanika#performance)
for full benchmarks.

---

## 6. Repository separation

lestraOS and lestramanika are **separate repositories** and must never
be cross-pushed:

| Repository | URL | Contents |
|---|---|---|
| **lestraOS** | `github.com/lee-muriithi-kingori/LestraOS` | x86_64 kernel, drivers, libc, userspace, networking, the in-kernel pickle selftest |
| **lestramanika** | `github.com/lee-muriithi-kingori/lestramanika` | the standalone GGUF engine: freestanding core + host fast path + CLI |

The lestramanika submodule pointer in lestraOS is the only coupling.
Changes to lestramanika land in the lestramanika repo first, then a
submodule bump + `sync_lestramanika.sh` run propagates the
kernel-compatible half into lestraOS.

---

## 7. Current versions

| Component | Version | Commit |
|---|---|---|
| lestraOS (this repo) | KE-36 | `34687a6` (pre-submodule) → this commit |
| lestramanika submodule | v0.4 alpha | `3e671ec` |
| In-kernel pickle (vendored) | synced to `3e671ec` | `kernel/ai/pickle.c` |

The kernel-compatible core at `3e671ec` adds (over the original KE-28
vendoring at `8d3300c`):

- **Metadata-only load** (`pickle_load_meta`) — parse GGUF header +
  metadata + tensor table without reading/dequantizing tensor data.
  Instant for multi-hundred-MB models.
- **On-demand dequant** (`pickle_dequant_tensor`) — dequantize a single
  tensor by index, lazily.
- **Raw tensor load** (`pickle_load_tensor_raw`) — load a tensor's
  on-disk bytes without dequantizing (used by the host fast path's
  quantized matmul kernels; available in-kernel for future use).
- **Optional `close` callback** on `pickle_io_t` — called by
  `pickle_free` if the io is owned.
- **Conditional `pickle.h` include** — the source now compiles verbatim
  into both the kernel and host builds with zero post-processing.

The kernel's `pickle_selftest()` boot-time behavior is unchanged — it
still prints `pickle: selftest OK, next token = 6` (the kernel bump
allocator + soft-float path produce the same result as before; the
demo model and forward pass are identical).
