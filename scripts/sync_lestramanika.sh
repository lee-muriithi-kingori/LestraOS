#!/usr/bin/env bash
# scripts/sync_lestramanika.sh — vendor the kernel-compatible half of
# lestramanika (https://github.com/lee-muriithi-kingori/lestramanika)
# into the lestraOS kernel tree.
#
# What this syncs (the kernel-compatible core only):
#
#   third_party/lestramanika/src/pickle.c            -> kernel/ai/pickle.c
#   third_party/lestramanika/src/pickle_softfp.c     -> kernel/ai/pickle_softfp.c
#   third_party/lestramanika/src/pickle_demo_gguf.c  -> kernel/ai/pickle_demo_gguf.c
#   third_party/lestramanika/src/pickle.h            -> kernel/include/lestra/pickle.h
#
# What this DOES NOT sync (host-only, requires POSIX + SSE/AVX + mmap):
#
#   src/pickle_fast.c        AVX-512 VNNI matmul + native forward path
#   src/pickle_fast.h        fast-path public API
#   src/pickle_tokenizer.c   Llama BPE tokenizer
#   src/pickle_host.c        POSIX shim (FILE* + mmap io callbacks)
#   src/pickle_cli.c         CLI frontend
#   src/pickle_selftest_main.c
#
# The host fast path lives exclusively in the lestramanika repo and is
# built there with `make`. lestraOS only consumes the freestanding core
# that runs under -mno-sse with the soft-float layer.
#
# The lestramanika source uses a conditional pickle.h include:
#
#   #ifdef PICKLE_KERNEL
#   #include <lestra/pickle.h>   /* kernel build: -Ikernel/include */
#   #else
#   #include "pickle.h"          /* host build: -Isrc */
#   #endif
#
# so the copy is verbatim — no sed-rewriting of include paths. The kernel
# build defines -DPICKLE_KERNEL (see the top-level Makefile CFLAGS), which
# also routes every other include (<lestra/types.h>, <lestra/printk.h>,
# <lestra/mm.h>) and the allocator (kmalloc vs malloc) to the kernel
# implementation.
#
# Usage:
#   ./scripts/sync_lestramanika.sh            # sync from pinned submodule
#   ./scripts/sync_lestramanika.sh --check    # exit 1 if drift detected, no writes
#
# Exit codes: 0 on success / no drift, 1 on error or drift (--check).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SUBMODULE="${REPO_ROOT}/third_party/lestramanika"
SRC="${SUBMODULE}/src"

# Destination paths inside the lestraOS tree.
declare -a PAIRS=(
  "pickle.c            kernel/ai/pickle.c"
  "pickle_softfp.c     kernel/ai/pickle_softfp.c"
  "pickle_demo_gguf.c  kernel/ai/pickle_demo_gguf.c"
  "pickle.h            kernel/include/lestra/pickle.h"
)

CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

# --- sanity: submodule must be initialised ----------------------------------
if [ ! -f "${SRC}/pickle.c" ]; then
  echo "error: lestramanika submodule not initialised at ${SUBMODULE}" >&2
  echo "       run: git submodule update --init --recursive" >&2
  exit 1
fi

# Report the submodule HEAD so the sync is reproducible.
SUB_HEAD="$(git -C "${SUBMODULE}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
SUB_SUBJ="$(git -C "${SUBMODULE}" log -1 --format='%s' 2>/dev/null || echo unknown)"
echo "lestramanika submodule HEAD: ${SUB_HEAD} — ${SUB_SUBJ}"
echo

# --- --check: compare without writing ---------------------------------------
if [ "${CHECK_ONLY}" -eq 1 ]; then
  drift=0
  for entry in "${PAIRS[@]}"; do
    set -- ${entry}
    src_file="${SRC}/$1"
    dst_file="${REPO_ROOT}/$2"
    if ! diff -q "${src_file}" "${dst_file}" >/dev/null 2>&1; then
      echo "DRIFT: $1  (submodule != $2)"
      drift=1
    fi
  done
  if [ "${drift}" -eq 0 ]; then
    echo "OK: kernel/ai pickle sources match submodule HEAD (${SUB_HEAD})."
    exit 0
  else
    echo
    echo "Run ./scripts/sync_lestramanika.sh to update." >&2
    exit 1
  fi
fi

# --- sync: copy verbatim -----------------------------------------------------
echo "Syncing kernel-compatible sources into the lestraOS tree..."
for entry in "${PAIRS[@]}"; do
  set -- ${entry}
  src_file="${SRC}/$1"
  dst_file="${REPO_ROOT}/$2"
  mkdir -p "$(dirname "${dst_file}")"
  cp "${src_file}" "${dst_file}"
  echo "  $1  ->  $2"
done

echo
echo "Done. Sources vendored from lestramanika ${SUB_HEAD}."
echo
echo "Next steps:"
echo "  1. Review the diff:  git diff kernel/ai/ kernel/include/lestra/pickle.h"
echo "  2. Rebuild kernel:   make clean && make all"
echo "  3. Boot-test:        make run  (verify 'pickle: selftest OK' on console)"
echo "  4. Commit the sync:  git add kernel/ai/ kernel/include/lestra/pickle.h"
echo "                       git commit -m 'ai: sync pickle from lestramanika ${SUB_HEAD}'"
