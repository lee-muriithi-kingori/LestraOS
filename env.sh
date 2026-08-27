# lestraOS no-root toolchain env (re-created after env reset)
export DEVTOOLS="${DEVTOOLS:-$HOME/.local/opt/devtools}"
export PATH="$DEVTOOLS/usr/bin:$PATH"
export LD_LIBRARY_PATH="$DEVTOOLS/usr/lib/x86_64-linux-gnu:$DEVTOOLS/usr/lib:${LD_LIBRARY_PATH:-}"
export QEMU_DATADIR="$DEVTOOLS/qemu-data"
export GRUB_DIR="$DEVTOOLS/usr/lib/grub/i386-pc"
# Use system gcc/ld (host x86_64) for kernel/userspace builds
export CC=gcc
export LD=ld
export AR=ar