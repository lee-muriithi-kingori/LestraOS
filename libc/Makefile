# Lestra OS - libc Makefile

CC ?= gcc
AR ?= ar
CFLAGS ?= -ffreestanding -O2 -Wall -Wextra
BUILD_DIR ?= ../build/libc

SRCS := src/string.c src/stdio.c src/stdlib.c src/unistd.c
OBJS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))

all: $(BUILD_DIR)/libc.a

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -Iinclude -c $< -o $@

$(BUILD_DIR)/libc.a: $(OBJS)
	@echo "  AR      $@"
	@$(AR) rcs $@ $^

clean:
	@rm -rf $(BUILD_DIR)

.PHONY: all clean
