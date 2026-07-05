# Lestra OS - User Space Makefile

CC ?= gcc
CFLAGS ?= -ffreestanding -O2 -Wall -Wextra
LDFLAGS ?= -nostdlib
BUILD_DIR ?= ../build/user
LIBC_DIR ?= ../build/libc

TARGETS := $(BUILD_DIR)/init $(BUILD_DIR)/shell $(BUILD_DIR)/sysinfo

all: $(TARGETS)

$(BUILD_DIR)/init: init/init.c $(LIBC_DIR)/libc.a | $(BUILD_DIR)
	@echo "  LD      $@"
	@$(CC) $(CFLAGS) -I../libc/include -Wl,-Ttext=0x400000 \
	    init/init.c $(LIBC_DIR)/libc.a -o $@

$(BUILD_DIR)/shell: shell/shell.c $(LIBC_DIR)/libc.a | $(BUILD_DIR)
	@echo "  LD      $@"
	@$(CC) $(CFLAGS) -I../libc/include -Wl,-Ttext=0x400000 \
	    shell/shell.c $(LIBC_DIR)/libc.a -o $@

$(BUILD_DIR)/sysinfo: bin/sysinfo.c $(LIBC_DIR)/libc.a | $(BUILD_DIR)
	@echo "  LD      $@"
	@$(CC) $(CFLAGS) -I../libc/include -Wl,-Ttext=0x400000 \
	    bin/sysinfo.c $(LIBC_DIR)/libc.a -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
