#!/bin/bash
# lestraOS cloud-mode smoke test.
# Boots the kernel in QEMU serial mode for 30s and verifies the boot
# reaches "kernel initialized successfully" without panicking.
set -e
export PATH="/home/z/.local/bin:$PATH"
export LD_LIBRARY_PATH="/home/z/.local/qemu-prefix/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH}"
cd /home/z/lestraOS

echo "[smoke] building ISO..."
make iso >/dev/null 2>&1 || make all >/dev/null 2>&1

# Build a serial-boot ISO
mkdir -p /tmp/isoserial/boot/grub
cp build/kernel.bin /tmp/isoserial/boot/kernel.bin
cp build/initrd.img /tmp/isoserial/boot/initrd.img
cat > /tmp/isoserial/boot/grub/grub.cfg <<'EOF'
set timeout=0
set default=0
menuentry "Lestra OS (Serial Cloud Mode)" {
    multiboot2 /boot/kernel.bin cloud serial
    module2 /boot/initrd.img initrd
    boot
}
EOF
grub-mkrescue -d /home/z/.local/qemu-prefix/usr/lib/grub/i386-pc -o /tmp/lestraos-serial.iso /tmp/isoserial >/dev/null 2>&1

echo "[smoke] booting (30s timeout)..."
timeout 30 qemu-system-x86_64 \
  -L /home/z/.local/qemu-prefix/usr/share/qemu \
  -cdrom /tmp/lestraos-serial.iso \
  -m 512M -cpu qemu64 \
  -nographic -boot d -no-reboot \
  -serial stdio -monitor none \
  -netdev user,id=net0 -device e1000,netdev=net0 \
  -name "Lestra OS [smoke]" > /tmp/lestra-smoke.log 2>&1 || true

sed -E 's/\x1b\[[0-9;?]*[a-zA-Z]//g; s/\r//g' /tmp/lestra-smoke.log > /tmp/lestra-smoke-clean.log

echo "[smoke] results:"
if grep -q "kernel initialized successfully" /tmp/lestra-smoke-clean.log; then
    echo "  PASS: kernel reached init"
else
    echo "  FAIL: kernel did not reach init"
    tail -30 /tmp/lestra-smoke-clean.log
    exit 1
fi
if grep -qi "KERNEL PANIC\|EXCEPTION:" /tmp/lestra-smoke-clean.log; then
    echo "  FAIL: kernel panicked"
    grep -i "PANIC\|EXCEPTION" /tmp/lestra-smoke-clean.log
    exit 1
else
    echo "  PASS: no panic"
fi
if grep -q "SECURITY AUDIT" /tmp/lestra-smoke-clean.log; then
    echo "  PASS: security audit print present"
    grep -A8 "SECURITY AUDIT" /tmp/lestra-smoke-clean.log
else
    echo "  WARN: security audit print not found"
fi
echo "[smoke] done."
