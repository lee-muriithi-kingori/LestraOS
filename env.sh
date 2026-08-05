# lestraOS no-root toolchain env (re-created after env reset)
export DEVTOOLS="/home/z/.local/opt/devtools/usr"
export PATH="/home/z/.local/opt/devtools/usr/bin:$PATH"
export LD_LIBRARY_PATH="/home/z/.local/opt/devtools/usr/lib:${LD_LIBRARY_PATH:-}"
export QEMU_DATADIR="/home/z/.local/opt/devtools/usr/share/qemu"
# Make + cross-gcc use system gcc (host x86_64) for kernel/userspace builds
export CC=gcc
export LD=ld
export AR=ar
