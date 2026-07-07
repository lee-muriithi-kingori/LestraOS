/*
 * Lestra OS - System Calls
 * Copyright (c) 2026 lestramk.org
 *
 * x86_64 syscall/sysret implementation.
 */

#include <lestra/types.h>
#include <lestra/syscall.h>
#include <lestra/printk.h>
#include <lestra/vga.h>
#include <lestra/serial.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <lestra/mm.h>
#include <lestra/sched.h>
#include <lestra/vfs.h>
#include <string.h>

/* Syscall entry point - defined in assembly */
extern void syscall_entry(void);

/* GDT selectors (must match kernel/include/lestra/gdt.h)
 * FIX: USER_CS was 0x23 (which is USER_DS | RPL3). The correct
 * user-mode code selector is USER_CS(0x18) | RPL3 = 0x1B. */
#ifndef USER_CS
#define USER_CS  0x1B  /* ring-3 code selector = 0x18 | RPL=3 */
#endif
#ifndef USER_DS
#define USER_DS  0x23  /* ring-3 data selector = 0x20 | RPL=3 */
#endif
#ifndef KERNEL_CS
#define KERNEL_CS 0x08 /* ring-0 code selector */
#endif
#ifndef KERNEL_DS
#define KERNEL_DS 0x10 /* ring-0 data selector */
#endif

static int64_t sys_exit(int64_t code) {
    pr_info("Process exited with code %d\n", (int)code);
    while (1) { hlt(); }
    return 0;
}

static int64_t sys_fork(void) {
    return -1; /* Not implemented */
}

static int64_t sys_read(int64_t fd, void* buf, size_t count) {
    (void)fd;
    if (!buf || count == 0) return -EFAULT;
    char* cbuf = (char*)buf;
    for (size_t i = 0; i < count; i++) {
        cbuf[i] = keyboard_getchar();
    }
    return (int64_t)count;
}

static int64_t sys_write(int64_t fd, const void* buf, size_t count) {
    (void)fd;
    if (!buf || count == 0) return -EFAULT;
    const char* cbuf = (const char*)buf;
    for (size_t i = 0; i < count; i++) {
        if (cbuf[i] == '\n') {
            vga_putchar('\r');
        }
        vga_putchar(cbuf[i]);
        serial_default_putchar(cbuf[i]);
    }
    return (int64_t)count;
}

static int64_t sys_open(const char* path, int flags) {
    (void)path; (void)flags;
    return -1; /* Not implemented */
}

static int64_t sys_close(int64_t fd) {
    (void)fd;
    return 0;
}

static int64_t sys_waitpid(int64_t pid, int* status, int options) {
    (void)pid; (void)status; (void)options;
    return -1;
}

static int64_t sys_execve(const char* path, char* const argv[], char* const envp[]) {
    (void)path; (void)argv; (void)envp;
    return -1;
}

static int64_t sys_getpid(void) {
    return 1; /* PID 1 for kernel shell */
}

static int64_t sys_brk(void* addr) {
    (void)addr;
    return -1;
}

static int64_t sys_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
    return -1;
}

static int64_t sys_munmap(void* addr, size_t length) {
    (void)addr; (void)length;
    return -1;
}

static int64_t sys_gettimeofday(void) {
    return (int64_t)timer_get_ms();
}

static int64_t sys_sleep(uint64_t ms) {
    uint64_t target = timer_get_ms() + ms;
    while (timer_get_ms() < target) {
        hlt();
    }
    return 0;
}

static int64_t sys_getcwd(char* buf, size_t size) {
    if (!buf || size == 0) return -EFAULT;
    const char* cwd = "/";
    size_t len = strlen(cwd) + 1;
    if (len > size) len = size;
    memcpy(buf, cwd, len);
    return 0;
}

static int64_t sys_chdir(const char* path) {
    (void)path;
    return -1;
}

static int64_t sys_mkdir(const char* path, uint32_t mode) {
    (void)path; (void)mode;
    return -1;
}

static int64_t sys_rmdir(const char* path) {
    (void)path;
    return -1;
}

static int64_t sys_stat(const char* path, void* st) {
    (void)path; (void)st;
    return -1;
}

static int64_t sys_lseek(int64_t fd, off_t offset, int whence) {
    (void)fd; (void)offset; (void)whence;
    return -1;
}

static int64_t sys_getdents(int64_t fd, void* dirp, size_t count) {
    (void)fd; (void)dirp; (void)count;
    return -1;
}

static int64_t sys_reboot(int64_t cmd) {
    if (cmd == 0) {
        printk("Shutting down...\n");
    } else {
        printk("Rebooting...\n");
    }
    outb(0x64, 0xFE);
    while (1) { hlt(); }
    return 0;
}

static int64_t sys_uname(void* buf) {
    if (!buf) return -EFAULT;
    /* Simple uname - just write "LestraOS" to buf */
    memset(buf, 0, 256);
    strcpy((char*)buf, "LestraOS");
    return 0;
}

void syscall_init(void) {
    /* Enable SCE (SYSCALL Enable) in EFER MSR */
    uint64_t efer = rdmsr(0xC0000080);
    wrmsr(0xC0000080, efer | 1);

    /* STAR: segment selectors for syscall/sysret
     * Bits 47:32 = SYSCALL CS (KERNEL_CS=0x08; SS = CS+8 = 0x10)
     * Bits 63:48 = SYSRET CS (USER_CS=0x18; SS = CS+8 = 0x20)
     * FIX: Previous value used USER_CS(0x23) in bits 63:48 which was wrong. */
    uint64_t star = ((uint64_t)USER_CS << 48) | ((uint64_t)KERNEL_CS << 32);
    wrmsr(0xC0000081, star);

    /* LSTAR: syscall handler entry point (64-bit) */
    wrmsr(0xC0000082, (uint64_t)syscall_entry);

    /* CSTAR: compatibility mode syscall handler (not used) */
    wrmsr(0xC0000083, 0);

    /* SFMASK: RFLAGS mask - clear IF on syscall */
    wrmsr(0xC0000084, 0x200);

    pr_info("Syscall interface initialized (SYSCALL/SYSRET)\n");
}

int64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a5;
    switch (num) {
        case SYS_EXIT:        return sys_exit((int64_t)a1);
        case SYS_FORK:        return sys_fork();
        case SYS_READ:        return sys_read((int64_t)a1, (void*)a2, a3);
        case SYS_WRITE:       return sys_write((int64_t)a1, (const void*)a2, a3);
        case SYS_OPEN:        return sys_open((const char*)a1, (int)a2);
        case SYS_CLOSE:       return sys_close((int64_t)a1);
        case SYS_WAITPID:     return sys_waitpid((int64_t)a1, (int*)a2, (int)a3);
        case SYS_EXECVE:      return sys_execve((const char*)a1, (char* const*)a2, (char* const*)a3);
        case SYS_GETPID:      return sys_getpid();
        case SYS_BRK:         return sys_brk((void*)a1);
        case SYS_MMAP:        return sys_mmap((void*)a1, a2, (int)a3, (int)a4, (int)a5, 0);
        case SYS_MUNMAP:      return sys_munmap((void*)a1, a2);
        case SYS_GETTIMEOFDAY: return sys_gettimeofday();
        case SYS_SLEEP:       return sys_sleep(a1);
        case SYS_GETCWD:      return sys_getcwd((char*)a1, a2);
        case SYS_CHDIR:       return sys_chdir((const char*)a1);
        case SYS_MKDIR:       return sys_mkdir((const char*)a1, (uint32_t)a2);
        case SYS_RMDIR:       return sys_rmdir((const char*)a1);
        case SYS_STAT:        return sys_stat((const char*)a1, (void*)a2);
        case SYS_LSEEK:       return sys_lseek((int64_t)a1, (off_t)a2, (int)a3);
        case SYS_GETDENTS:    return sys_getdents((int64_t)a1, (void*)a2, a3);
        case SYS_REBOOT:      return sys_reboot((int64_t)a1);
        case SYS_UNAME:       return sys_uname((void*)a1);
        default:
            pr_warn("Unknown syscall: %u\n", (unsigned)num);
            return -ENOSYS;
    }
}
