# Lestra OS - User Space Makefile
#
# FIX: user ELFs must be built with the same no-SSE / no-default-libs
# flags as the kernel, otherwise ld may pull in crt0/crti from the
# host glibc and the resulting binary will crash on first FPU/SSE
# instruction (the kernel runs with CR4.OSFXSR cleared).

CC ?= gcc
CFLAGS ?= -ffreestanding -O2 -Wall -Wextra \
          -nostartfiles -nodefaultlibs -nostdlib \
          -fno-pie -fno-stack-protector \
          -mno-red-zone -mcmodel=large \
          -mno-mmx -mno-sse -mno-sse2 -fomit-frame-pointer
LDFLAGS ?= -nostdlib -nostartfiles -nodefaultlibs -no-pie
BUILD_DIR ?= ../build/user
LIBC_DIR ?= ../build/libc

TARGETS := $(BUILD_DIR)/init $(BUILD_DIR)/shell $(BUILD_DIR)/sysinfo

all: $(TARGETS)

# Each user ELF is linked with an explicit -e _start so ld's default entry
# heuristic doesn't land at the first byte of .text (which could be any
# helper function). All three sources now define _start themselves.
$(BUILD_DIR)/init: init/init.c $(LIBC_DIR)/libc.a | $(BUILD_DIR)
	@echo "  LD      $@"
	@$(CC) $(CFLAGS) -I../libc/include -Wl,-Ttext=0x400000 -Wl,-e,_start \
	    init/init.c $(LIBC_DIR)/libc.a -o $@

$(BUILD_DIR)/shell: shell/shell.c $(LIBC_DIR)/libc.a | $(BUILD_DIR)
	@echo "  LD      $@"
	@$(CC) $(CFLAGS) -I../libc/include -Wl,-Ttext=0x400000 -Wl,-e,_start \
	    shell/shell.c $(LIBC_DIR)/libc.a -o $@

$(BUILD_DIR)/sysinfo: bin/sysinfo.c $(LIBC_DIR)/libc.a | $(BUILD_DIR)
	@echo "  LD      $@"
	@$(CC) $(CFLAGS) -I../libc/include -Wl,-Ttext=0x400000 -Wl,-e,_start \
	    bin/sysinfo.c $(LIBC_DIR)/libc.a -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
