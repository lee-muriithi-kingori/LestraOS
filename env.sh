# lestraOS no-root toolchain env (re-created after env reset)
export DEVTOOLS="/home/z/.local/opt/devtools"
export PATH="/home/z/.local/opt/devtools/usr/bin:$PATH"
export LD_LIBRARY_PATH="/home/z/.local/opt/devtools/usr/lib/x86_64-linux-gnu:/home/z/.local/opt/devtools/usr/lib:${LD_LIBRARY_PATH:-}"
export QEMU_DATADIR="/home/z/.local/opt/devtools/qemu-data"
export GRUB_DIR="/home/z/.local/opt/devtools/usr/lib/grub/i386-pc"
# Use system gcc/ld (host x86_64) for kernel/userspace builds
export CC=gcc
export LD=ld
export AR=ar
