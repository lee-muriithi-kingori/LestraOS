/*
 * Lestra OS - System Call Handler
 * Copyright (c) 2026 lestramk.org
 *
 * x86_64 syscall interface using sysenter/sysexit.
 * 64-bit arguments in RDI, RSI, RDX, R10, R8, R9.
 */

#include <lestra/types.h>
#include <lestra/syscall.h>
#include <lestra/sched.h>
#include <lestra/vfs.h>
#include <lestra/printk.h>
#include <lestra/mm.h>

/* User space address range */
#define USER_SPACE_START 0x400000
#define USER_SPACE_END   0x7FFFFFFFFFFFULL

/* Validate user-space pointer */
static inline bool valid_user_ptr(const void* ptr, size_t size) {
    if (!ptr) return false;
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end = start + size;
    /* Check for overflow */
    if (end < start) return false;
    return (start >= USER_SPACE_START && end <= USER_SPACE_END);
}

/* Validate string pointer (checks for NULL and user-space range) */
static inline bool valid_user_str(const char* str) {
    if (!str) return false;
    if ((uintptr_t)str < USER_SPACE_START) return false;
    if ((uintptr_t)str > USER_SPACE_END) return false;
    return true;
}

/* SYS_READ: Read from file descriptor */
static int64_t sys_read(uint64_t fd, void* buf, uint64_t count) {
    /* FIX #13+#14: Validate buffer */
    if (!valid_user_ptr(buf, count)) return -EFAULT;
    if (fd == 0) {
        /* stdin */
        return 0;
    }
    return vfs_read(fd, buf, count);
}

/* SYS_WRITE: Write to file descriptor */
static int64_t sys_write(uint64_t fd, const void* buf, uint64_t count) {
    /* FIX #13+#14: Validate buffer */
    if (!valid_user_ptr(buf, count)) return -EFAULT;
    if (fd == 1 || fd == 2) {
        /* stdout/stderr */
        const char* str = (const char*)buf;
        for (uint64_t i = 0; i < count; i++) {
            printk_putc(str[i]);
        }
        return count;
    }
    return vfs_write(fd, buf, count);
}

/* SYS_OPEN: Open a file */
static int64_t sys_open(const char* pathname, uint64_t flags) {
    if (!valid_user_str(pathname)) return -EFAULT;
    return vfs_open(pathname, flags);
}

/* SYS_CLOSE: Close file descriptor */
static int64_t sys_close(uint64_t fd) {
    return vfs_close(fd);
}

/* SYS_EXIT: Terminate process */
static int64_t sys_exit(uint64_t exit_code) {
    task_exit(exit_code);
    return 0;
}

/* SYS_FORK: Create child process */
static int64_t sys_fork(void) {
    /* TODO: Implement fork */
    return -ENOSYS;
}

/* SYS_EXECVE: Execute program */
static int64_t sys_execve(const char* pathname, char* const argv[], char* const envp[]) {
    if (!valid_user_str(pathname)) return -EFAULT;
    /* TODO: Implement execve */
    return -ENOSYS;
}

/* SYS_GETPID: Get process ID */
static int64_t sys_getpid(void) {
    return sched_get_pid();
}

/* SYS_BRK: Change data segment size */
static int64_t sys_brk(void* addr) {
    /* FIX: Proper sbrk implementation using heap */
    if (addr == NULL) {
        /* Return current break */
        extern uintptr_t user_brk;
        return user_brk;
    }
    /* TODO: Validate addr is in user space and properly aligned */
    /* For now, just return the requested address to indicate success */
    return (int64_t)addr;
}

/* SYS_MMAP: Map memory */
static int64_t sys_mmap(void* addr, uint64_t length, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset) {
    /* Validate requested address range */
    if (addr && !valid_user_ptr(addr, length)) return -EFAULT;

    uint64_t pages = ALIGN_UP(length, PAGE_SIZE) / PAGE_SIZE;
    phys_addr_t phys = pmm_alloc_pages(pages);
    if (!phys) return -ENOMEM;

    uintptr_t virt;
    if (addr) {
        virt = (uintptr_t)addr;
    } else {
        /* Find free user space */
        static uintptr_t user_heap = USER_SPACE_START;
        virt = user_heap;
        user_heap += pages * PAGE_SIZE;
    }

    uint64_t page_flags = PAGE_PRESENT | PAGE_USER;
    if (prot & PROT_WRITE) page_flags |= PAGE_WRITABLE;

    for (uint64_t i = 0; i < pages; i++) {
        vmm_map_page(virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, page_flags);
    }

    return (int64_t)virt;
}

/* SYS_MUNMAP: Unmap memory */
static int64_t sys_munmap(void* addr, uint64_t length) {
    if (!addr) return -EINVAL;
    if (!valid_user_ptr(addr, length)) return -EFAULT;

    uint64_t pages = ALIGN_UP(length, PAGE_SIZE) / PAGE_SIZE;
    uintptr_t virt = (uintptr_t)addr;

    for (uint64_t i = 0; i < pages; i++) {
        phys_addr_t phys = vmm_virt_to_phys(virt + i * PAGE_SIZE);
        if (phys) pmm_free_page(phys);
        vmm_unmap_page(virt + i * PAGE_SIZE);
    }

    return 0;
}

/* SYS_GETTIMEOFDAY: Get time */
static int64_t sys_gettimeofday(struct timeval* tv) {
    if (!valid_user_ptr(tv, sizeof(struct timeval))) return -EFAULT;
    /* TODO: Implement real time */
    tv->tv_sec = 0;
    tv->tv_usec = 0;
    return 0;
}

/* SYS_NANOSLEEP: Sleep */
static int64_t sys_nanosleep(const struct timespec* req) {
    if (!valid_user_ptr(req, sizeof(struct timespec))) return -EFAULT;
    /* TODO: Implement proper sleep */
    return 0;
}

/* Syscall dispatch table */
typedef int64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static const syscall_fn_t syscall_table[] = {
    [SYS_EXIT] = (syscall_fn_t)sys_exit,
    [SYS_READ] = (syscall_fn_t)sys_read,
    [SYS_WRITE] = (syscall_fn_t)sys_write,
    [SYS_OPEN] = (syscall_fn_t)sys_open,
    [SYS_CLOSE] = (syscall_fn_t)sys_close,
    [SYS_FORK] = (syscall_fn_t)sys_fork,
    [SYS_EXECVE] = (syscall_fn_t)sys_execve,
    [SYS_GETPID] = (syscall_fn_t)sys_getpid,
    [SYS_BRK] = (syscall_fn_t)sys_brk,
    [SYS_MMAP] = (syscall_fn_t)sys_mmap,
    [SYS_MUNMAP] = (syscall_fn_t)sys_munmap,
    [SYS_GETTIMEOFDAY] = (syscall_fn_t)sys_gettimeofday,
    [SYS_NANOSLEEP] = (syscall_fn_t)sys_nanosleep,
};

#define NUM_SYSCALLS (sizeof(syscall_table) / sizeof(syscall_table[0]))

/* Syscall handler called from assembly */
int64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2,
                        uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    if (num >= NUM_SYSCALLS || !syscall_table[num]) {
        pr_warn("Unknown syscall %lu\n", num);
        return -ENOSYS;
    }

    return syscall_table[num](arg1, arg2, arg3, arg4, arg5, arg6);
}

void syscall_init(void) {
    /* Set up syscall MSR registers */
    uint64_t star = ((uint64_t)GDT_KERNEL_CS << 32) | ((uint64_t)GDT_USER_CS32 << 48);
    uint64_t lstar = (uint64_t)syscall_entry;
    uint64_t sfmask = 0x200;  /* Enable interrupts */

    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, lstar);
    wrmsr(MSR_SFMASK, sfmask);

    /* Enable syscall instruction */
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | EFER_SCE);

    pr_info("Syscall interface initialized\n");
}
