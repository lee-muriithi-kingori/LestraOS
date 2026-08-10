# Lestra OS - Main Build Makefile (FIXED)
# Copyright (c) 2026 lestramk.org

# Build configuration
DEBUG ?= 0
OPTIMIZE ?= 2
ARCH := x86_64

# FIX: Cross-compiler detection - prefer x86_64-elf-gcc, fall back to system gcc.
# Previous logic fell back to x86_64-lestra- which doesn't exist anywhere.
CROSS_PREFIX := $(shell which x86_64-elf-gcc >/dev/null 2>&1 && echo x86_64-elf- || echo "")
HAS_CROSS := $(shell if [ -n "$(CROSS_PREFIX)" ]; then echo yes; else echo no; fi)

ifeq ($(HAS_CROSS),yes)
	CC := $(CROSS_PREFIX)gcc
	CXX := $(CROSS_PREFIX)g++
	LD := $(CROSS_PREFIX)ld
	AR := $(CROSS_PREFIX)ar
	OBJCOPY := $(CROSS_PREFIX)objcopy
	STRIP := $(CROSS_PREFIX)strip
else
	CC := gcc
	CXX := g++
	LD := ld
	AR := ar
	OBJCOPY := objcopy
	STRIP := strip
endif

AS := nasm

# Detect GRUB i386-pc modules directory so grub-mkrescue can find
# boot.img, eltorito.img, etc. Without this, grub-mkrescue silently
# produces a non-bootable ISO (no El Torito boot record).
GRUB_MODULES_DIR ?= $(shell for d in /usr/lib/grub/i386-pc /usr/lib/grub2/i386-pc $(HOME)/opt/cross/lib/grub/i386-pc /usr/local/lib/grub/i386-pc /home/z/.local/qemu-prefix/usr/lib/grub/i386-pc $(HOME)/.local/opt/devtools/usr/lib/grub/i386-pc ; do \
	if [ -f "$$d/boot.img" ]; then echo $$d; break; fi; \
done)

# Flags
CFLAGS := -ffreestanding -O$(OPTIMIZE) -Wall -Wextra \
		  -nostdlib -nostartfiles -nodefaultlibs \
		  -I$(CURDIR)/kernel/include -I$(CURDIR)/libc/include -m64 -mno-red-zone -mcmodel=large \
		  -mno-mmx -mno-sse -mno-sse2 -fomit-frame-pointer -fstack-protector-strong \
		  -DPICKLE_KERNEL

ifeq ($(DEBUG),1)
	CFLAGS += -g -DDEBUG -DLESTRA_DEBUG
else
	CFLAGS += -DNDEBUG
endif

ASFLAGS := -f elf64 -F dwarf
# FIX: add -m elf_x86_64 to specify emulation explicitly
LDFLAGS := -nostdlib --no-dynamic-linker -z max-page-size=0x1000 -m elf_x86_64

# Directories
BUILD_DIR := build
ISO_DIR := iso
GRUB_DIR := $(ISO_DIR)/boot/grub

# Output files
KERNEL_BIN := $(BUILD_DIR)/kernel.bin
KERNEL_ISO := $(BUILD_DIR)/lestraos.iso
INITRD := $(BUILD_DIR)/initrd.img
LESTRA_IMG := $(BUILD_DIR)/lestraos.img   # raw HDD/USB image (stage1 + kernel + initrd)
KERNEL_RAW := $(BUILD_DIR)/kernel.raw     # flat binary of kernel.bin (for raw-disk boot)

# Source files
BOOT_SRCS := $(wildcard boot/*.asm)
ARCH_SRCS := $(wildcard kernel/arch/$(ARCH)/*.c kernel/arch/$(ARCH)/*.asm)
CORE_SRCS := $(wildcard kernel/core/*.c)
MM_SRCS := $(wildcard kernel/mm/*.c)
SCHED_SRCS := $(wildcard kernel/sched/*.c)
SCHED_ASM := $(wildcard kernel/sched/*.asm)
DRIVER_SRCS := $(wildcard kernel/drivers/*/*.c kernel/drivers/*/*.asm)
SYSCALL_SRCS := $(wildcard kernel/syscall/*.c kernel/syscall/*.asm)
UI_SRCS := $(wildcard kernel/ui/*.c)
PKG_SRCS := $(wildcard pkg/*.c) $(wildcard kernel/pkg/*.c)
GUI_SRCS := $(wildcard kernel/gui/*.c)
INPUT_SRCS := kernel/input.c
AI_SRCS := $(wildcard kernel/ai/*.c)
NET_SRCS := $(wildcard kernel/net/*.c)
EXEC_SRCS := $(wildcard kernel/exec/*.c)
TTS_SRCS := $(wildcard kernel/audio/*.c)
EXEC_ASM := kernel/exec/elf_jump_to_user.asm

# ldso.c (dynamic ELF linker — Linux compat) is automatically picked
# up by the wildcard above.
SYS_SRCS := $(wildcard kernel/sys/*.c)
POWER_SRCS := $(wildcard kernel/drivers/power/*.c)
CLOCK_SRCS := $(wildcard kernel/drivers/clock/*.c)
SENSOR_SRCS := $(wildcard kernel/drivers/sensor/*.c)
FS_SRCS := $(wildcard kernel/fs/*.c) $(wildcard kernel/fs/ext2/*.c)
SPLASH_SRCS := kernel/splash.c
ACPI_SRCS := $(wildcard kernel/acpi/*.c)

# Object files
BOOT_OBJS := $(patsubst boot/%.asm,$(BUILD_DIR)/boot/%.o,$(filter-out boot/stage1.asm,$(BOOT_SRCS)))
STAGE1_BIN := $(BUILD_DIR)/boot/stage1.bin
ARCH_OBJS := $(patsubst kernel/arch/$(ARCH)/%.c,$(BUILD_DIR)/arch/%.o,$(filter %.c,$(ARCH_SRCS))) \
			 $(patsubst kernel/arch/$(ARCH)/%.asm,$(BUILD_DIR)/arch/%.o,$(filter %.asm,$(ARCH_SRCS)))
CORE_OBJS := $(patsubst kernel/core/%.c,$(BUILD_DIR)/core/%.o,$(CORE_SRCS))
MM_OBJS := $(patsubst kernel/mm/%.c,$(BUILD_DIR)/mm/%.o,$(MM_SRCS))
SCHED_OBJS := $(patsubst kernel/sched/%.c,$(BUILD_DIR)/sched/%.o,$(SCHED_SRCS)) \
			   $(patsubst kernel/sched/%.asm,$(BUILD_DIR)/sched/%.o,$(SCHED_ASM))
DRIVER_OBJS := $(patsubst kernel/drivers/char/%.c,$(BUILD_DIR)/drivers/char/%.o,$(wildcard kernel/drivers/char/*.c)) \
			   $(patsubst kernel/drivers/block/%.c,$(BUILD_DIR)/drivers/block/%.o,$(wildcard kernel/drivers/block/*.c)) \
			   $(patsubst kernel/drivers/audio/%.c,$(BUILD_DIR)/drivers/audio/%.o,$(wildcard kernel/drivers/audio/*.c)) \
			   $(patsubst kernel/drivers/pci/%.c,$(BUILD_DIR)/drivers/pci/%.o,$(wildcard kernel/drivers/pci/*.c)) \
			   $(patsubst kernel/drivers/net/%.c,$(BUILD_DIR)/drivers/net/%.o,$(wildcard kernel/drivers/net/*.c)) \
			   $(patsubst kernel/drivers/apic/%.c,$(BUILD_DIR)/drivers/apic/%.o,$(wildcard kernel/drivers/apic/*.c))
SYSCALL_OBJS := $(patsubst kernel/syscall/%.c,$(BUILD_DIR)/syscall/%.o,$(filter %.c,$(SYSCALL_SRCS))) \
				$(patsubst kernel/syscall/%.asm,$(BUILD_DIR)/syscall/%.o,$(filter %.asm,$(SYSCALL_SRCS)))
UI_OBJS := $(patsubst kernel/ui/%.c,$(BUILD_DIR)/ui/%.o,$(UI_SRCS))
PKG_OBJS := $(patsubst pkg/%.c,$(BUILD_DIR)/pkg/%.o,$(filter pkg/%.c,$(PKG_SRCS))) \
			   $(patsubst kernel/pkg/%.c,$(BUILD_DIR)/kpkg/%.o,$(filter kernel/pkg/%.c,$(PKG_SRCS)))
GUI_OBJS := $(patsubst kernel/gui/%.c,$(BUILD_DIR)/gui/%.o,$(GUI_SRCS))
INPUT_OBJS := $(patsubst kernel/%.c,$(BUILD_DIR)/%.o,$(INPUT_SRCS))
AI_OBJS := $(patsubst kernel/ai/%.c,$(BUILD_DIR)/ai/%.o,$(AI_SRCS))
NET_OBJS := $(patsubst kernel/net/%.c,$(BUILD_DIR)/net/%.o,$(NET_SRCS))
EXEC_OBJS := $(patsubst kernel/exec/%.c,$(BUILD_DIR)/exec/%.o,$(EXEC_SRCS)) $(BUILD_DIR)/exec/elf_jump_to_user.o
TTS_OBJS := $(patsubst kernel/audio/%.c,$(BUILD_DIR)/audio/%.o,$(TTS_SRCS))
SYS_OBJS := $(patsubst kernel/sys/%.c,$(BUILD_DIR)/sys/%.o,$(SYS_SRCS))
POWER_OBJS := $(patsubst kernel/drivers/power/%.c,$(BUILD_DIR)/drivers/power/%.o,$(POWER_SRCS))
CLOCK_OBJS := $(patsubst kernel/drivers/clock/%.c,$(BUILD_DIR)/drivers/clock/%.o,$(CLOCK_SRCS))
SENSOR_OBJS := $(patsubst kernel/drivers/sensor/%.c,$(BUILD_DIR)/drivers/sensor/%.o,$(SENSOR_SRCS))
FS_OBJS := $(patsubst kernel/fs/%.c,$(BUILD_DIR)/fs/%.o,$(filter-out kernel/fs/ext2/%.c,$(filter kernel/fs/%.c,$(FS_SRCS)))) \
			   $(patsubst kernel/fs/ext2/%.c,$(BUILD_DIR)/fs/ext2/%.o,$(filter kernel/fs/ext2/%.c,$(FS_SRCS)))
SPLASH_OBJS := $(patsubst kernel/%.c,$(BUILD_DIR)/%.o,$(SPLASH_SRCS))
ACPI_OBJS := $(patsubst kernel/acpi/%.c,$(BUILD_DIR)/acpi/%.o,$(ACPI_SRCS))

ALL_KERNEL_OBJS := $(BOOT_OBJS) $(ARCH_OBJS) $(CORE_OBJS) $(MM_OBJS) \
				   $(SCHED_OBJS) $(FS_OBJS) $(DRIVER_OBJS) $(SYSCALL_OBJS) $(UI_OBJS) \
				   $(PKG_OBJS) $(AI_OBJS) $(NET_OBJS) \
				   $(GUI_OBJS) $(INPUT_OBJS) $(SPLASH_OBJS) $(EXEC_OBJS) \
				   $(SYS_OBJS) $(POWER_OBJS) $(CLOCK_OBJS) $(SENSOR_OBJS) $(TTS_OBJS) $(ACPI_OBJS)

# Phony targets
.PHONY: all clean run run-debug run-kernel smoke test iso kernel libc userspace initrd img install docs help

# Default target
all: kernel libc userspace initrd iso

# Create build directories
$(BUILD_DIR)/boot $(BUILD_DIR)/arch $(BUILD_DIR)/core $(BUILD_DIR)/mm \
$(BUILD_DIR)/sched $(BUILD_DIR)/fs $(BUILD_DIR)/fs/ext2 \
$(BUILD_DIR)/drivers/char $(BUILD_DIR)/drivers/block $(BUILD_DIR)/drivers/pci \
$(BUILD_DIR)/drivers/net $(BUILD_DIR)/drivers/audio $(BUILD_DIR)/drivers/power \
$(BUILD_DIR)/drivers/clock $(BUILD_DIR)/drivers/sensor $(BUILD_DIR)/drivers/apic \
$(BUILD_DIR)/syscall $(BUILD_DIR)/libc $(BUILD_DIR)/user $(BUILD_DIR)/ui \
$(BUILD_DIR)/pkg $(BUILD_DIR)/kpkg $(BUILD_DIR)/ai \
$(BUILD_DIR)/net $(BUILD_DIR)/audio $(BUILD_DIR)/gui $(BUILD_DIR)/exec \
$(BUILD_DIR)/acpi \
$(BUILD_DIR)/sys $(GRUB_DIR):
	@mkdir -p $@

# Boot objects
$(BUILD_DIR)/boot/boot.o: boot/boot.asm | $(BUILD_DIR)/boot
	@echo "  AS      $<"
	@$(AS) $(ASFLAGS) $< -o $@

# Stage1 (16-bit MBR bootloader) - built separately if needed
$(BUILD_DIR)/boot/stage1.bin: boot/stage1.asm | $(BUILD_DIR)/boot
	@echo "  AS      $< (bin)"
	@$(AS) -f bin $< -o $@

# Architecture objects
$(BUILD_DIR)/arch/%.o: kernel/arch/$(ARCH)/%.c | $(BUILD_DIR)/arch
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/arch/%.o: kernel/arch/$(ARCH)/%.asm | $(BUILD_DIR)/arch
	@echo "  AS      $<"
	@$(AS) $(ASFLAGS) $< -o $@

# Core kernel objects
$(BUILD_DIR)/core/%.o: kernel/core/%.c | $(BUILD_DIR)/core
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Memory management objects
$(BUILD_DIR)/mm/%.o: kernel/mm/%.c | $(BUILD_DIR)/mm
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Scheduler objects
$(BUILD_DIR)/sched/%.o: kernel/sched/%.c | $(BUILD_DIR)/sched
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/sched/%.o: kernel/sched/%.asm | $(BUILD_DIR)/sched
	@echo "  AS      $<"
	@$(AS) $(ASFLAGS) $< -o $@

# File system objects
$(BUILD_DIR)/fs/%.o: kernel/fs/%.c | $(BUILD_DIR)/fs
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Driver objects
$(BUILD_DIR)/drivers/char/%.o: kernel/drivers/char/%.c | $(BUILD_DIR)/drivers/char
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/block/%.o: kernel/drivers/block/%.c | $(BUILD_DIR)/drivers/block
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/drivers/pci/%.o: kernel/drivers/pci/%.c | $(BUILD_DIR)/drivers/pci
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Syscall objects
$(BUILD_DIR)/syscall/%.o: kernel/syscall/%.c | $(BUILD_DIR)/syscall
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/syscall/%.o: kernel/syscall/%.asm | $(BUILD_DIR)/syscall
	@echo "  AS      $<"
	@$(AS) $(ASFLAGS) $< -o $@

# UI objects
$(BUILD_DIR)/ui/%.o: kernel/ui/%.c | $(BUILD_DIR)/ui
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Package manager objects
$(BUILD_DIR)/pkg/%.o: pkg/%.c | $(BUILD_DIR)/pkg
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# AI subsystem objects
$(BUILD_DIR)/ai/%.o: kernel/ai/%.c | $(BUILD_DIR)/ai
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Net stack objects
$(BUILD_DIR)/net/%.o: kernel/net/%.c | $(BUILD_DIR)/net
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Net driver objects (drivers/net/)
$(BUILD_DIR)/drivers/net/%.o: kernel/drivers/net/%.c | $(BUILD_DIR)/drivers/net
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# APIC driver objects (drivers/apic/) - KE-23 LAPIC + IOAPIC
$(BUILD_DIR)/drivers/apic/%.o: kernel/drivers/apic/%.c | $(BUILD_DIR)/drivers/apic
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Filesystem objects (kernel/fs/)
$(BUILD_DIR)/fs/%.o: kernel/fs/%.c | $(BUILD_DIR)/fs
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# ext2 filesystem objects (kernel/fs/ext2/)
$(BUILD_DIR)/fs/ext2/%.o: kernel/fs/ext2/%.c | $(BUILD_DIR)/fs/ext2
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# TTS engine (kernel/audio/)
$(BUILD_DIR)/audio/%.o: kernel/audio/%.c | $(BUILD_DIR)/audio
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Power driver (kernel/drivers/power/)
$(BUILD_DIR)/drivers/power/%.o: kernel/drivers/power/%.c | $(BUILD_DIR)/drivers/power
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Clock driver (kernel/drivers/clock/)
$(BUILD_DIR)/drivers/clock/%.o: kernel/drivers/clock/%.c | $(BUILD_DIR)/drivers/clock
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Sensor driver (kernel/drivers/sensor/)
$(BUILD_DIR)/drivers/sensor/%.o: kernel/drivers/sensor/%.c | $(BUILD_DIR)/drivers/sensor
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# ACPI (kernel/acpi/)
$(BUILD_DIR)/acpi/%.o: kernel/acpi/%.c | $(BUILD_DIR)/acpi
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Sys (kernel/sys/)
$(BUILD_DIR)/sys/%.o: kernel/sys/%.c | $(BUILD_DIR)/sys
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Audio driver (kernel/drivers/audio/)
$(BUILD_DIR)/drivers/audio/%.o: kernel/drivers/audio/%.c | $(BUILD_DIR)/drivers/audio
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# ELF loader (kernel/exec/)
$(BUILD_DIR)/exec/%.o: kernel/exec/%.c | $(BUILD_DIR)/exec
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/exec/%.o: kernel/exec/%.asm | $(BUILD_DIR)/exec
	@echo "  AS      $<"
	@$(AS) $(ASFLAGS) $< -o $@

# GUI objects
$(BUILD_DIR)/gui/%.o: kernel/gui/%.c | $(BUILD_DIR)/gui
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Kernel package objects (kernel/pkg/)
$(BUILD_DIR)/kpkg/%.o: kernel/pkg/%.c | $(BUILD_DIR)/kpkg
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Input subsystem (kernel/input.c)
$(BUILD_DIR)/%.o: kernel/%.c | $(BUILD_DIR)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Link kernel
# FIX: include $(PKG_OBJS) $(AI_OBJS) in link
$(KERNEL_BIN): $(ALL_KERNEL_OBJS) kernel/arch/$(ARCH)/linker.ld | $(BUILD_DIR)
	@echo "  LD      $@"
	@$(LD) -T kernel/arch/$(ARCH)/linker.ld -e _start $(LDFLAGS) -o $@ $(ALL_KERNEL_OBJS) $(BUILD_DIR)/libc/libc.a
	@echo "  Kernel build complete: $@ ($(shell stat -c%s $@ 2>/dev/null || echo ?) bytes)"

kernel: libc $(KERNEL_BIN)

# libc build
libc: | $(BUILD_DIR)/libc
	@echo "  Building libc..."
	@$(MAKE) -C libc BUILD_DIR=../$(BUILD_DIR)/libc CC="$(CC)" CFLAGS="$(filter-out -fstack-protector-strong,$(CFLAGS))" AR="$(AR)"

# User space build
userspace: libc | $(BUILD_DIR)/user
	@echo "  Building user space..."
	@$(MAKE) -C user BUILD_DIR=../$(BUILD_DIR)/user CC="$(CC)" CFLAGS="$(filter-out -fstack-protector-strong,$(CFLAGS))"

# Initrd generation
initrd: userspace | $(BUILD_DIR)
	@echo "  Creating initrd..."
	@python3 scripts/mkinitrd.py $(INITRD) $(BUILD_DIR)/user/init $(BUILD_DIR)/user/shell $(BUILD_DIR)/user/sysinfo 2>/dev/null || \
	echo "  WARNING: Could not create initrd (check python3 and user binaries)"

# ISO generation
iso: kernel initrd | $(GRUB_DIR)
	@echo "  Building ISO..."
	@mkdir -p $(ISO_DIR)/boot/grub
	@cp $(KERNEL_BIN) $(ISO_DIR)/boot/kernel.bin
	@cp $(INITRD) $(ISO_DIR)/boot/initrd.img 2>/dev/null || true
	@cp boot/grub.cfg $(GRUB_DIR)/grub.cfg
	@if [ -n "$(GRUB_MODULES_DIR)" ]; then \
	grub-mkrescue -d "$(GRUB_MODULES_DIR)" -o $(KERNEL_ISO) $(ISO_DIR); \
		elif command -v grub2-mkrescue >/dev/null 2>&1; then \
	grub2-mkrescue -o $(KERNEL_ISO) $(ISO_DIR); \
		else \
	echo "  WARNING: grub-mkrescue / grub2-mkrescue not found, ISO not built"; \
		fi
	@echo "  ISO build complete: $(KERNEL_ISO)"

# Raw-disk image (bootable on bare metal / USB stick without GRUB).
# Layout: stage1.bin (512 B) | kernel.raw (padded to 64 KB) | initrd.img
# stage1 loads 127 sectors from LBA 1 into 0x10000 and jumps to _start
# (which lives at 0x10E00 because .text starts at file offset 0x1000).
$(KERNEL_RAW): $(KERNEL_BIN)
	@echo "  OBJCOPY $< -> $@"
	@objcopy -O binary $< $@

img: kernel initrd $(STAGE1_BIN) $(KERNEL_RAW)
	@echo "  Building raw disk image: $(LESTRA_IMG)"
	@rm -f $(LESTRA_IMG)
	@dd if=/dev/zero of=$(LESTRA_IMG) bs=1M count=2 2>/dev/null
	@dd if=$(STAGE1_BIN) of=$(LESTRA_IMG) conv=notrunc 2>/dev/null
	@dd if=$(KERNEL_RAW) of=$(LESTRA_IMG) seek=1 bs=512 conv=notrunc 2>/dev/null
	@dd if=$(INITRD) of=$(LESTRA_IMG) seek=128 bs=512 conv=notrunc 2>/dev/null
	@echo "  Raw image complete: $(LESTRA_IMG) ($(shell stat -c%s $(LESTRA_IMG) 2>/dev/null || echo ?) bytes)"

# QEMU targets
run: all
	@echo "  Starting Lestra OS in QEMU..."
	@qemu-system-x86_64 -cdrom $(KERNEL_ISO) -m 512M -cpu qemu64 \
		 -nographic -boot d -no-reboot \
		 -netdev user,id=net0 -device e1000,netdev=net0 \
		 -name "Lestra OS"

run-cloud: all
	@echo "  Starting Lestra OS in QEMU (cloud/serial mode)..."
	@qemu-system-x86_64 -cdrom $(KERNEL_ISO) -m 512M -cpu qemu64 \
		 -nographic -boot d -no-reboot \
		 -serial stdio -monitor none \
		 -netdev user,id=net0 -device e1000,netdev=net0 \
		 -name "Lestra OS [cloud]"

# QEMU firmware (SeaBIOS) lookup dir. The devtools prefix is created by
# scripts/setup-devtools.sh; fall back to the system path if absent.
QEMU_FW_DIR ?= $(or $(wildcard $(HOME)/.local/opt/devtools/usr/share/qemu),/usr/share/qemu)

# Headless test ISO: same kernel+initrd as the release ISO, but with a
# grub.cfg that boots straight into cloud/serial mode (timeout=0, no menu,
# no gfxterm) so it works under `qemu -nographic` without hanging at a menu.
$(BUILD_DIR)/lestraos-test.iso: kernel initrd boot/grub-test.cfg | $(GRUB_DIR)
	@echo "  Building headless test ISO..."
	@mkdir -p $(ISO_DIR)/boot/grub
	@cp $(KERNEL_BIN) $(ISO_DIR)/boot/kernel.bin
	@cp $(INITRD) $(ISO_DIR)/boot/initrd.img
	@cp boot/grub-test.cfg $(GRUB_DIR)/grub.cfg
	@if [ -n "$(GRUB_MODULES_DIR)" ]; then \
		grub-mkrescue -d "$(GRUB_MODULES_DIR)" -o $@ $(ISO_DIR); \
	else grub-mkrescue -o $@ $(ISO_DIR); fi 2>/dev/null
	@echo "  Test ISO: $@"

smoke: $(BUILD_DIR)/lestraos-test.iso
	@echo "  Running cloud-mode smoke test (20s)..."
	@timeout 20 qemu-system-x86_64 -L $(QEMU_FW_DIR) \
		 -cdrom $(BUILD_DIR)/lestraos-test.iso -m 512M -cpu qemu64 \
		 -nographic -boot d -no-reboot \
		 -serial stdio -monitor none \
		 -netdev user,id=net0 -device e1000,netdev=net0 \
		 -name "Lestra OS [smoke]" > /tmp/lestra-smoke.log 2>&1 || true
	@echo "  Smoke log: /tmp/lestra-smoke.log ($$(wc -l < /tmp/lestra-smoke.log) lines)"
	@grep -q "kernel initialized successfully" /tmp/lestra-smoke.log && echo "  PASS: kernel reached init" || echo "  FAIL: kernel did not reach init"
	@grep -q "pickle: selftest OK" /tmp/lestra-smoke.log && echo "  PASS: in-kernel pickle selftest" || echo "  FAIL: pickle selftest not seen"
	@grep -q "CLOUD/VPS SERVER MODE" /tmp/lestra-smoke.log && echo "  PASS: cloud mode entered" || echo "  WARN: cloud mode banner not seen"
	@grep -q "DHCP: ACK" /tmp/lestra-smoke.log && echo "  PASS: DHCP lease acquired" || echo "  WARN: DHCP lease not seen"

# `make test` — strict version of smoke: exits non-zero if any marker fails.
# Intended for CI. Builds the headless test ISO, boots it, and asserts the
# golden-path boot markers appear in the serial log.
test: $(BUILD_DIR)/lestraos-test.iso
	@echo "  Running strict boot test (20s)..."
	@timeout 20 qemu-system-x86_64 -L $(QEMU_FW_DIR) \
		 -cdrom $(BUILD_DIR)/lestraos-test.iso -m 512M -cpu qemu64 \
		 -nographic -boot d -no-reboot \
		 -serial stdio -monitor none \
		 -netdev user,id=net0 -device e1000,netdev=net0 \
		 -name "Lestra OS [test]" > /tmp/lestra-test.log 2>&1 || true
	@echo "  Test log: /tmp/lestra-test.log"
	@fail=0; \
	grep -q "kernel initialized successfully" /tmp/lestra-test.log || { echo "  FAIL: kernel did not reach init"; fail=1; }; \
	grep -q "pickle: selftest OK" /tmp/lestra-test.log || { echo "  FAIL: pickle selftest not seen"; fail=1; }; \
	grep -q "CLOUD/VPS SERVER MODE" /tmp/lestra-test.log || { echo "  FAIL: cloud mode not entered"; fail=1; }; \
	grep -q "DHCP: ACK" /tmp/lestra-test.log || { echo "  FAIL: DHCP lease not acquired"; fail=1; }; \
	[ $$fail -eq 0 ] && echo "  ALL CHECKS PASSED" || { echo "  TEST FAILED"; exit 1; }

run-debug: all
	@echo "  Starting QEMU with GDB server..."
	@qemu-system-x86_64 -cdrom $(KERNEL_ISO) -m 4096M -cpu qemu64 \
		 -vga std -serial stdio -boot d -no-reboot -no-shutdown \
		 -s -S -name "Lestra OS [DEBUG]"

run-kernel: kernel
	@echo "  Starting kernel directly in QEMU..."
	@qemu-system-x86_64 -kernel $(KERNEL_BIN) -m 4096M -cpu qemu64 \
		 -vga std -serial stdio -append "debug" -no-reboot -no-shutdown

# Clean
clean:
	@echo "  Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR) $(ISO_DIR)
	@$(MAKE) -C libc clean BUILD_DIR=../$(BUILD_DIR)/libc 2>/dev/null || true
	@$(MAKE) -C user clean BUILD_DIR=../$(BUILD_DIR)/user 2>/dev/null || true

# Documentation
docs:
	@echo "  Building documentation..."
	@mkdir -p $(BUILD_DIR)/docs
	@cp docs/*.md $(BUILD_DIR)/docs/ 2>/dev/null || true

# Help
help:
	@echo "Lestra OS Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all         - Build everything (kernel, libc, userspace, ISO)"
	@echo "  kernel      - Build kernel only"
	@echo "  libc        - Build C library"
	@echo "  userspace   - Build user space programs"
	@echo "  initrd      - Create initial ramdisk"
	@echo "  iso         - Build bootable ISO (GRUB + El Torito)"
	@echo "  img         - Build raw HDD/USB image (stage1 MBR, no GRUB)"
	@echo "  run         - Run in QEMU (4GB RAM)"
	@echo "  run-debug   - Run with GDB server"
	@echo "  run-kernel  - Run kernel directly"
	@echo "  clean       - Clean all build artifacts"
	@echo "  docs        - Build documentation"
	@echo "  help        - Show this help"
	@echo ""
	@echo "Options:"
	@echo "  DEBUG=1     - Enable debug output"
	@echo "  OPTIMIZE=N  - Set optimization level (0-3)"
	@echo ""
	@echo "Setup:"
	@echo "  1. ./build/cross-compiler.sh    (builds x86_64-elf-gcc)"
	@echo "  2. export PATH=$$HOME/opt/cross/bin:$$PATH"
	@echo "  3. make all"
	@echo "  4. make run"
