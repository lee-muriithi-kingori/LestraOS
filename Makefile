# Lestra OS - Main Build Makefile
# Copyright (c) 2026 lestramk.org

# Build configuration
DEBUG ?= 0
OPTIMIZE ?= 2
TARGET := x86_64-lestra
ARCH := x86_64

# Cross-compiler tools
CC := $(TARGET)-gcc
CXX := $(TARGET)-g++
AS := nasm
LD := $(TARGET)-ld
AR := $(TARGET)-ar
OBJCOPY := $(TARGET)-objcopy
STRIP := $(TARGET)-strip

# Flags
CFLAGS := -ffreestanding -O$(OPTIMIZE) -Wall -Wextra -fno-exceptions \
          -fno-rtti -nostdlib -nostartfiles -nodefaultlibs \
          -Ikernel/include -Ilibc/include -mno-red-zone -mcmodel=large \
          -mno-mmx -mno-sse -mno-sse2 -fomit-frame-pointer

ifeq ($(DEBUG),1)
CFLAGS += -g -DDEBUG -DLESTRA_DEBUG
else
CFLAGS += -DNDEBUG
endif

ASFLAGS := -f elf64 -F dwarf
LDFLAGS := -nostdlib -lgcc

# Directories
BUILD_DIR := build
ISO_DIR := iso
GRUB_DIR := $(ISO_DIR)/boot/grub

# Output files
KERNEL_BIN := $(BUILD_DIR)/kernel.bin
KERNEL_ISO := $(BUILD_DIR)/lestraos.iso
INITRD := $(BUILD_DIR)/initrd.img

# Source files
BOOT_SRCS := $(wildcard boot/*.asm)
ARCH_SRCS := $(wildcard kernel/arch/$(ARCH)/*.c kernel/arch/$(ARCH)/*.asm)
CORE_SRCS := $(wildcard kernel/core/*.c)
MM_SRCS := $(wildcard kernel/mm/*.c)
SCHED_SRCS := $(wildcard kernel/sched/*.c)
FS_SRCS := $(wildcard kernel/fs/*.c)
DRIVER_SRCS := $(wildcard kernel/drivers/*/*.c kernel/drivers/*/*.asm)
SYSCALL_SRCS := $(wildcard kernel/syscall/*.c)

# Object files
BOOT_OBJS := $(patsubst boot/%.asm,$(BUILD_DIR)/boot/%.o,$(BOOT_SRCS))
ARCH_OBJS := $(patsubst kernel/arch/$(ARCH)/%.c,$(BUILD_DIR)/arch/%.o,$(filter %.c,$(ARCH_SRCS))) \
             $(patsubst kernel/arch/$(ARCH)/%.asm,$(BUILD_DIR)/arch/%.o,$(filter %.asm,$(ARCH_SRCS)))
CORE_OBJS := $(patsubst kernel/core/%.c,$(BUILD_DIR)/core/%.o,$(CORE_SRCS))
MM_OBJS := $(patsubst kernel/mm/%.c,$(BUILD_DIR)/mm/%.o,$(MM_SRCS))
SCHED_OBJS := $(patsubst kernel/sched/%.c,$(BUILD_DIR)/sched/%.o,$(SCHED_SRCS))
FS_OBJS := $(patsubst kernel/fs/%.c,$(BUILD_DIR)/fs/%.o,$(FS_SRCS))
DRIVER_OBJS := $(patsubst kernel/drivers/char/%.c,$(BUILD_DIR)/drivers/char/%.o,$(wildcard kernel/drivers/char/*.c)) \
               $(patsubst kernel/drivers/block/%.c,$(BUILD_DIR)/drivers/block/%.o,$(wildcard kernel/drivers/block/*.c)) \
               $(patsubst kernel/drivers/pci/%.c,$(BUILD_DIR)/drivers/pci/%.o,$(wildcard kernel/drivers/pci/*.c))
SYSCALL_OBJS := $(patsubst kernel/syscall/%.c,$(BUILD_DIR)/syscall/%.o,$(SYSCALL_SRCS))

ALL_KERNEL_OBJS := $(BOOT_OBJS) $(ARCH_OBJS) $(CORE_OBJS) $(MM_OBJS) \
                   $(SCHED_OBJS) $(FS_OBJS) $(DRIVER_OBJS) $(SYSCALL_OBJS)

# Phony targets
.PHONY: all clean run run-debug iso kernel libc userspace initrd install docs

# Default target
all: kernel libc userspace initrd iso

# Create build directories
$(BUILD_DIR)/boot $(BUILD_DIR)/arch $(BUILD_DIR)/core $(BUILD_DIR)/mm \
$(BUILD_DIR)/sched $(BUILD_DIR)/fs $(BUILD_DIR)/drivers/char \
$(BUILD_DIR)/drivers/block $(BUILD_DIR)/drivers/pci $(BUILD_DIR)/syscall \
$(BUILD_DIR)/libc $(BUILD_DIR)/user $(GRUB_DIR):
	@mkdir -p $@

# Boot objects
$(BUILD_DIR)/boot/%.o: boot/%.asm | $(BUILD_DIR)/boot
	@echo "  AS      $<"
	@$(AS) $(ASFLAGS) $< -o $@

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

# Link kernel
$(KERNEL_BIN): $(ALL_KERNEL_OBJS) kernel/arch/$(ARCH)/linker.ld | $(BUILD_DIR)
	@echo "  LD      $@"
	@$(LD) -T kernel/arch/$(ARCH)/linker.ld $(LDFLAGS) -o $@ $(ALL_KERNEL_OBJS)
	@echo "  Kernel build complete: $@"

kernel: $(KERNEL_BIN)

# libc build
libc: | $(BUILD_DIR)/libc
	@echo "  Building libc..."
	@$(MAKE) -C libc BUILD_DIR=../$(BUILD_DIR)/libc CC="$(CC)" CFLAGS="$(CFLAGS)" AR="$(AR)"

# User space build
userspace: libc | $(BUILD_DIR)/user
	@echo "  Building user space..."
	@$(MAKE) -C user BUILD_DIR=../$(BUILD_DIR)/user CC="$(CC)" CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)"

# Initrd generation
initrd: userspace | $(BUILD_DIR)
	@echo "  Creating initrd..."
	@./build/mkinitrd.sh $(BUILD_DIR)/user $(INITRD)

# ISO generation
iso: kernel initrd | $(GRUB_DIR)
	@echo "  Building ISO..."
	@cp $(KERNEL_BIN) $(ISO_DIR)/boot/kernel.bin
	@cp $(INITRD) $(ISO_DIR)/boot/initrd.img
	@cp boot/grub.cfg $(GRUB_DIR)/grub.cfg
	@grub-mkrescue -o $(KERNEL_ISO) $(ISO_DIR) 2>/dev/null || \
	 grub2-mkrescue -o $(KERNEL_ISO) $(ISO_DIR) 2>/dev/null || \
	 echo "  WARNING: grub-mkrescue not found, ISO not built"
	@echo "  ISO build complete: $(KERNEL_ISO)"

# QEMU targets
run: all
	@echo "  Starting Lestra OS in QEMU..."
	@qemu-system-x86_64 -cdrom $(KERNEL_ISO) -m 4096M -cpu qemu64 \
	 -vga std -serial stdio -boot d -no-reboot -no-shutdown \
	 -device qemu-xhci -name "Lestra OS"

run-debug: all
	@echo "  Starting QEMU with GDB server..."
	@qemu-system-x86_64 -cdrom $(KERNEL_ISO) -m 4096M -cpu qemu64 \
	 -vga std -serial stdio -boot d -no-reboot -no-shutdown \
	 -s -S -name "Lestra OS [DEBUG]"

run-kernel: kernel
	@echo "  Starting kernel directly in QEMU..."
	@qemu-system-x86_64 -kernel $(KERNEL_BIN) -m 4096M -cpu qemu64 \
	 -vga std -serial stdio -append "debug" -no-reboot

# Clean
clean:
	@echo "  Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR) $(ISO_DIR)/boot/kernel.bin $(ISO_DIR)/boot/initrd.img
	@$(MAKE) -C libc clean BUILD_DIR=../$(BUILD_DIR)/libc 2>/dev/null || true
	@$(MAKE) -C user clean BUILD_DIR=../$(BUILD_DIR)/user 2>/dev/null || true

# Documentation
docs:
	@echo "  Building documentation..."
	@mkdir -p $(BUILD_DIR)/docs
	@cp docs/*.md $(BUILD_DIR)/docs/ 2>/dev/null || true

# Install (for package building)
install: all
	@echo "  Installing Lestra OS..."
	@./installer/install.sh $(BUILD_DIR)

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
	@echo "  iso         - Build bootable ISO"
	@echo "  run         - Run in QEMU (4GB RAM)"
	@echo "  run-debug   - Run with GDB server"
	@echo "  run-kernel  - Run kernel directly"
	@echo "  clean       - Clean all build artifacts"
	@echo "  docs        - Build documentation"
	@echo "  install     - Install Lestra OS"
	@echo "  help        - Show this help"
	@echo ""
	@echo "Options:"
	@echo "  DEBUG=1     - Enable debug output"
	@echo "  OPTIMIZE=N  - Set optimization level (0-3)"
