/*
 * Lestra OS - Userspace test program
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * This is a REAL userspace program that runs in ring 3. It tests the
 * syscall interface by writing "Hello from userspace!" to stdout (fd 1),
 * which the kernel routes to the VGA screen + serial port.
 *
 * Compile: x86_64-elf-gcc -O2 -ffreestanding -nostdlib -nostartfiles \
 *            -I../libc/include -Wl,-Ttext=0x400000 -Wl,-e,_start \
 *            -o hello.elf hello.c ../build/libc/libc.a
 *
 * Add to initrd: python3 scripts/mkinitrd.py initrd.img hello.elf
 *
 * Run in LestraOS: exec /hello.elf
 */

#include <unistd.h>
#include <string.h>

static const char* msg = "Hello from userspace! (ring 3)\n";
static const char* msg2 = "If you see this, ELF loading + ring 3 works!\n";

void _start(void) {
    /* SYS_WRITE = 3, fd=1 (stdout), buf=msg, len=strlen */
    write(1, msg, strlen(msg));
    write(1, msg2, strlen(msg2));

    /* SYS_EXIT = 0, status=0 */
    _exit(0);
}
