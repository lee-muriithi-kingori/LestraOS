/*
 * Lestra OS - System Call Implementation
 * Copyright (c) 2026 lestramk.org
 */

#include <lestra/types.h>
#include <lestra/syscall.h>
#include <lestra/printk.h>
#include <lestra/vfs.h>
#include <lestra/sched.h>
#include <lestra/mm.h>
#include <lestra/timer.h>
#include <lestra/panic.h>

/* UTS name for uname */
static struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
} uts_name = {
    .sysname = "LestraOS",
    .nodename = "lestra",
    .release = "1.0.0",
    .version = "#1 SMP PREEMPT",
    .machine = "x86_64"
};

static int64_t sys_exit(int code) {
    task_exit(code);
    return 0;
}

static int64_t sys_read(int fd, void* buf, size_t count) {
    return vfs_read(fd, buf, count);
}

static int64_t sys_write(int fd, const void* buf, size_t count) {
    if (fd == 1 || fd == 2) {
        /* stdout/stderr - print to VGA and serial */
        const char* str = (const char*)buf;
        for (size_t i = 0; i < count; i++) {
            if (str[i] == '\n') {
                vga_putchar('\r');
            }
            vga_putchar(str[i]);
            serial_default_putchar(str[i]);
        }
        return count;
    }
    return vfs_write(fd, buf, count);
}

static int64_t sys_open(const char* path, int flags) {
    return vfs_open(path, flags);
}

static int64_t sys_close(int fd) {
    return vfs_close(fd);
}

static int64_t sys_getpid(void) {
    struct task* t = task_current();
    return t ? t->pid : 0;
}

static int64_t sys_brk(void* addr) {
    (void)addr;
    /* Simple implementation - return current heap end */
    return 0;
}

static int64_t sys_sleep(uint64_t ms) {
    task_sleep(ms);
    return 0;
}

static int64_t sys_stat(const char* path, void* st) {
    return vfs_stat(path, (struct stat*)st);
}

static int64_t sys_gettimeofday(void* tv) {
    (void)tv;
    /* Return timer value */
    return timer_get_ms();
}

static int64_t sys_reboot(int cmd) {
    if (cmd == 0) {
        /* Shutdown */
        pr_info("System shutting down...\n");
        cli();
        hlt();
    } else if (cmd == 1) {
        /* Reboot via keyboard controller */
        pr_info("System rebooting...\n");
        uint8_t good = 0x02;
        while (good & 0x02) {
            good = inb(0x64);
        }
        outb(0x64, 0xFE);
        hlt();
    }
    return 0;
}

static int64_t sys_uname(void* buf) {
    struct utsname* dst = (struct utsname*)buf;
    *dst = uts_name;
    return 0;
}

/* Syscall dispatch table */
typedef int64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static syscall_fn_t syscall_table[] = {
    [SYS_EXIT] = (syscall_fn_t)sys_exit,
    [SYS_READ] = (syscall_fn_t)sys_read,
    [SYS_WRITE] = (syscall_fn_t)sys_write,
    [SYS_OPEN] = (syscall_fn_t)sys_open,
    [SYS_CLOSE] = (syscall_fn_t)sys_close,
    [SYS_GETPID] = (syscall_fn_t)sys_getpid,
    [SYS_BRK] = (syscall_fn_t)sys_brk,
    [SYS_SLEEP] = (syscall_fn_t)sys_sleep,
    [SYS_STAT] = (syscall_fn_t)sys_stat,
    [SYS_REBOOT] = (syscall_fn_t)sys_reboot,
    [SYS_UNAME] = (syscall_fn_t)sys_uname,
};

int64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a4;
    (void)a5;
    
    if (num >= SYS_MAX || !syscall_table[num]) {
        return -ENOSYS;
    }
    
    return syscall_table[num](a1, a2, a3, 0, 0);
}

/* Assembly syscall entry */
__asm__(
    ".global syscall_entry\n"
    "syscall_entry:\n"
    "    swapgs\n"
    "    push rcx\n"       /* Save return address */
    "    push r11\n"       /* Save rflags */
    "    push rbp\n"
    "    push rbx\n"
    "    push r12\n"
    "    push r13\n"
    "    push r14\n"
    "    push r15\n"
    "    mov rcx, r10\n"   /* 4th arg */
    "    call syscall_dispatch\n"
    "    pop r15\n"
    "    pop r14\n"
    "    pop r13\n"
    "    pop r12\n"
    "    pop rbx\n"
    "    pop rbp\n"
    "    pop r11\n"
    "    pop rcx\n"
    "    swapgs\n"
    "    sysretq\n"
);

void syscall_init(void) {
    /* Set up MSR registers for syscall/sysret */
    wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);     /* EFER.SCE */
    wrmsr(0xC0000081, ((uint64_t)0x08 << 32) |     /* SYSRET CS */
                      ((uint64_t)0x1B << 48));      /* SYSCALL CS */
    wrmsr(0xC0000082, (uint64_t)syscall_entry);     /* Syscall handler */
    
    pr_debug("Syscall interface initialized\n");
}

extern void syscall_entry(void);
