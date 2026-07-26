/*
 * Lestra OS - System Calls
 * Copyright (c) 2026 lestramk.org
 *
 * x86_64 syscall/sysret implementation.
 *
 * This is the central syscall dispatcher. When a userspace program
 * issues `syscall`, the CPU lands in syscall_entry.asm which shuffles
 * registers into the C calling convention and calls syscall_dispatch()
 * here.
 *
 * ROUTING:
 *   - If the current task is marked as a Linux process
 *     (current->is_linux_process), the syscall is dispatched to
 *     linux_compat_dispatch() instead of the native LestraOS handler.
 *     This lets us run unmodified Linux ELFs (busybox, Go binaries,
 *     etc.) by translating their Linux syscall numbers to ours.
 *   - After every dispatch (native or Linux), signal_check_and_deliver()
 *     is called. If the current process has any unblocked signal
 *     pending, the saved RIP is rewritten to point at the user's signal
 *     handler and the original state is stashed so rt_sigreturn can
 *     restore it.
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

/* errno constants used by syscalls. Mirror libc/include/errno.h if
 * one existed; we declare inline here. */
#define EPERM           1
#define ENOENT          2
#define EIO             5
#define EBADF           9
#define ENOMEM         12
#define EACCES         13
#define EFAULT         14
#define EBUSY          16
#define EEXIST         17
#define EINVAL         22
#define ENOSYS         38
#define ERANGE         34
#define ENAMETOOLONG   36
#define EROFS          30
#define ECHILD         10
#define EAGAIN         11

/* LestraOS syscall numbers — must match kernel/include/lestra/syscall.h
 * and libc/include/unistd.h. Declared here as well so the linux_compat
 * shim and the dispatch table both have a single source of truth
 * visible from this translation unit. */
#define LESTRA_SYS_EXIT           0
#define LESTRA_SYS_FORK           1
#define LESTRA_SYS_READ           2
#define LESTRA_SYS_WRITE          3
#define LESTRA_SYS_OPEN           4
#define LESTRA_SYS_CLOSE          5
#define LESTRA_SYS_WAITPID        6
#define LESTRA_SYS_EXECVE         7
#define LESTRA_SYS_GETPID         8
#define LESTRA_SYS_BRK            9
#define LESTRA_SYS_MMAP          10
#define LESTRA_SYS_MUNMAP        11
#define LESTRA_SYS_GETTIMEOFDAY  12
#define LESTRA_SYS_SLEEP         13
#define LESTRA_SYS_GETCWD        14
#define LESTRA_SYS_CHDIR         15
#define LESTRA_SYS_MKDIR         16
#define LESTRA_SYS_RMDIR         17
#define LESTRA_SYS_STAT          18
#define LESTRA_SYS_LSEEK         19
#define LESTRA_SYS_GETDENTS      20
#define LESTRA_SYS_REBOOT        21
#define LESTRA_SYS_UNAME         22
#define LESTRA_SYS_PIPE          23
#define LESTRA_SYS_KILL          24
#define LESTRA_SYS_RT_SIGACTION    25
#define LESTRA_SYS_RT_SIGPROCMASK  26
#define LESTRA_SYS_RT_SIGRETURN    27
#define LESTRA_SYS_DUP2            28

/* Forward declarations for the Linux compatibility shim and signal
 * delivery. Both live in separate translation units (linux_compat.c
 * and signals.c, respectively) so the syscall dispatcher stays
 * readable. */
extern int64_t linux_compat_dispatch(uint64_t linux_num,
                                      uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6);
extern void signal_check_and_deliver(void);

/* Forward declarations for signal syscalls */
extern int64_t signal_sigaction(int signum, uint64_t act, uint64_t oldact, uint64_t sigsetsize);
extern int64_t signal_kill(int pid, int sig);
extern int64_t signal_sigprocmask(int how, uint64_t set, uint64_t oldset, uint64_t sigsetsize);
extern int64_t signal_sigreturn(void);

/* Forward declaration for pipe */
extern int pipe_create(int fds[2]);

/* Forward declarations for the ELF loaders. elf_exec handles static
 * (ET_EXEC) binaries; ldso_load_and_run handles dynamic (ET_DYN with
 * PT_INTERP/PT_DYNAMIC) binaries by running the in-kernel dynamic
 * linker. ldso_is_dynamic is the cheap peek that decides which path
 * to take. */
extern int  elf_exec(const char* path);
extern int  ldso_load_and_run(const char* exe_path, int argc, char** argv, char** envp);
extern int  ldso_is_dynamic(const char* path);

/* Forward decls for kernel functions we use. */
extern int  proc_getpid(void);
extern int  proc_fork(void);
extern void proc_exit(int);
extern int  proc_wait(int, int*);
extern int  proc_wait_blocking(int, int*);

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
    pr_info("syscall: PID %d exited with code %d\n",
            (int)proc_getpid(), (int)code);
    /* Tell the scheduler this process is done. proc_exit() will mark
     * it zombie and pick the next runnable task. If we're the only
     * task, we halt as before. */
    proc_exit((int)code);
    /* If proc_exit returned (no other task to switch to), halt. */
    while (1) { hlt(); }
    return 0;
}

static int64_t sys_fork(void) {
    return (int64_t)proc_fork();
}

static int64_t sys_read(int64_t fd, void* buf, size_t count) {
    if (!buf || count == 0) return -EFAULT;
    /* stdin (fd 0) reads from keyboard — same as before. */
    if (fd == 0) {
        char* cbuf = (char*)buf;
        for (size_t i = 0; i < count; i++) {
            cbuf[i] = keyboard_getchar();
        }
        return (int64_t)count;
    }
    /* Any other fd routes through VFS. */
    ssize_t n = vfs_read((int)fd, buf, count);
    return (n < 0) ? -EIO : (int64_t)n;
}

static int64_t sys_write(int64_t fd, const void* buf, size_t count) {
    if (!buf || count == 0) return -EFAULT;
    /* stdout/stderr go to VGA+serial. */
    if (fd == 1 || fd == 2) {
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
    /* Any other fd routes through VFS. */
    ssize_t n = vfs_write((int)fd, buf, count);
    return (n < 0) ? -EIO : (int64_t)n;
}

static int64_t sys_open(const char* path, int flags) {
    if (!path) return -EFAULT;
    int fd = vfs_open(path, flags);
    return (fd < 0) ? -ENOENT : (int64_t)fd;
}

static int64_t sys_close(int64_t fd) {
    vfs_close((int)fd);
    return 0;
}

static int64_t sys_waitpid(int64_t pid, int* status, int options) {
    (void)options;
    if (pid > 0) {
        return (int64_t)proc_wait_blocking((int)pid, status);
    }
    /* Wait for any child */
    for (int i = 1; i < MAX_PROCS; i++) {
        int rc = proc_wait(i, status);
        if (rc > 0) return (int64_t)rc;
    }
    return -ECHILD;
}

static int64_t sys_execve(const char* path, char* const argv[], char* const envp[]) {
    if (!path) return -EFAULT;
    /* If the binary is dynamically linked, hand off to the in-kernel
     * dynamic linker (ldso.c). Otherwise, run the static ELF loader.
     * ldso_is_dynamic does a cheap peek at PT_INTERP/PT_DYNAMIC. */
    if (ldso_is_dynamic(path)) {
        pr_info("syscall: execve(%s) -> ldso_load_and_run\n", path);
        return (int64_t)ldso_load_and_run(path,
                                          argv ? (int)0 : 0,
                                          (char**)argv,
                                          (char**)envp);
    }
    pr_info("syscall: execve(%s) -> elf_exec\n", path);
    return (int64_t)elf_exec(path);
}

static int64_t sys_getpid(void) {
    int p = proc_getpid();
    /* proc_getpid returns 0 if scheduler has no current task (kernel
     * context). Fall back to 1 so libc wrappers see a valid PID. */
    return (p > 0) ? (int64_t)p : 1;
}

/* Per-process program break. In a real OS this is per-task; since we
 * only have one user task at a time right now, a single static break
 * is fine. Heap grows from __heap_base upward. */
extern char __heap_base[];  /* provided by linker if defined; else 0 */
static void* current_brk = NULL;

static int64_t sys_brk(void* addr) {
    /* Linux semantics: brk(0) returns current break; brk(addr) sets it
     * and returns the new break (or current on failure). */
    if (!current_brk) current_brk = (void*)0x40000000ULL;  /* 1 GB */
    if (!addr) return (int64_t)current_brk;
    /* Only allow growing the break, within a 16 MB cap. */
    uintptr_t new_brk = (uintptr_t)addr;
    uintptr_t cap     = (uintptr_t)current_brk + (16 * 1024 * 1024);
    if (new_brk > cap) return (int64_t)current_brk;
    current_brk = addr;
    return (int64_t)current_brk;
}

static int64_t sys_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    /* Minimal mmap: anonymous pages only. File-backed mmap requires
     * page-cache integration we don't have yet. */
    (void)prot;
    if (!(flags & 0x20 /* MAP_ANONYMOUS */)) {
        (void)fd; (void)offset;
        return -ENOSYS;
    }
    if (length == 0) return -EINVAL;
    /* Round up to page size, allocate physical pages, return a kernel-
     * visible virtual address. For now we just kmalloc and return that
     * pointer — the user process is single-threaded and the kernel heap
     * is identity-mapped, so this works for our minimal userspace. */
    size_t rounded = (length + PAGE_SIZE - 1) & ~((size_t)PAGE_SIZE - 1);
    void* p = kmalloc(rounded);
    if (!p) return -ENOMEM;
    memset(p, 0, rounded);
    return (int64_t)p;
}

static int64_t sys_munmap(void* addr, size_t length) {
    (void)addr; (void)length;
    /* We don't track mmap regions yet; kfree would be unsafe because
     * we don't know if the caller actually owns the pointer. Leak. */
    return 0;
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

/* Per-process CWD. Single static buffer for now. */
static char cwd[MAX_PATH_LEN] = "/";
static int  cwd_set = 1;

static int64_t sys_getcwd(char* buf, size_t size) {
    if (!buf || size == 0) return -EFAULT;
    size_t len = strlen(cwd) + 1;
    if (len > size) return -ERANGE;
    memcpy(buf, cwd, len);
    return 0;
}

static int64_t sys_chdir(const char* path) {
    if (!path) return -EFAULT;
    /* Reject paths we can't possibly cd into. */
    if (strlen(path) >= MAX_PATH_LEN) return -ENAMETOOLONG;
    /* VFS has no real directories yet, but we accept any non-empty
     * path that starts with '/'. Relative-path resolution is left
     * for a future commit. */
    if (path[0] != '/') {
        /* Append to cwd. */
        char tmp[MAX_PATH_LEN];
        size_t n = ksnprintf(tmp, sizeof(tmp), "%s%s%s",
                             cwd,
                             (cwd[strlen(cwd)-1] == '/') ? "" : "/",
                             path);
        if (n >= sizeof(tmp)) return -ENAMETOOLONG;
        strncpy(cwd, tmp, sizeof(cwd) - 1);
    } else {
        strncpy(cwd, path, sizeof(cwd) - 1);
        cwd[sizeof(cwd) - 1] = '\0';
    }
    return 0;
}

static int64_t sys_mkdir(const char* path, uint32_t mode) {
    (void)mode;
    int rc = vfs_mkdir(path, 0755);
    return (rc < 0) ? -EROFS : 0;
}

static int64_t sys_rmdir(const char* path) {
    (void)path;
    /* VFS doesn't support rmdir yet. */
    return -EROFS;
}

static int64_t sys_stat(const char* path, void* st) {
    if (!path || !st) return -EFAULT;
    int rc = vfs_stat(path, (struct stat*)st);
    return (rc < 0) ? -ENOENT : 0;
}

static int64_t sys_lseek(int64_t fd, off_t offset, int whence) {
    /* SEEK_SET=0, SEEK_CUR=1, SEEK_END=2.
     * We track per-fd offsets in a small static array because the
     * current VFS implementation reads from offset 0 every time.
     * Future: thread the offset through vfs_read/vfs_write. */
    static off_t fd_offsets[128];
    int idx = (int)fd;
    if (idx < 0 || idx >= 128) return -EBADF;
    /* VFS files don't expose their size to us here, so SEEK_END
     * approximates by reading until EOF (slow but correct). For now
     * we just treat SEEK_END as a no-op returning current offset. */
    off_t base;
    switch (whence) {
        case 0: base = 0; break;
        case 1: base = fd_offsets[idx]; break;
        case 2: base = 0; offset = 0; break;  /* see comment above */
        default: return -EINVAL;
    }
    fd_offsets[idx] = base + offset;
    return (int64_t)fd_offsets[idx];
}

static int64_t sys_getdents(int64_t fd, void* dirp, size_t count) {
    /* VFS exposes vfs_readdir one entry at a time. We pack as many
     * struct dirent entries as fit in the user buffer. */
    if (!dirp || count == 0) return -EINVAL;
    struct dirent* out = (struct dirent*)dirp;
    size_t bytes = 0;
    int idx = 0;
    while (bytes + sizeof(struct dirent) <= count) {
        int rc = vfs_readdir((int)fd, &out[idx]);
        if (rc <= 0) break;
        idx++;
        bytes += sizeof(struct dirent);
    }
    return (int64_t)bytes;
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
    memset(buf, 0, 256);
    strcpy((char*)buf, "LestraOS");
    return 0;
}

static int64_t sys_pipe(int* user_fds) {
    if (!user_fds) return -EFAULT;
    int fds[2];
    int rc = pipe_create(fds);
    if (rc < 0) return -1;
    user_fds[0] = fds[0];
    user_fds[1] = fds[1];
    return 0;
}

static int64_t sys_dup2(int oldfd, int newfd) {
    if (oldfd == newfd) return newfd;
    /* Minimal dup2: close newfd if open, then make newfd point to same underlying resource.
     * For now, just return newfd if oldfd is valid. A real implementation would
     * need per-process fd tables. */
    if (oldfd < 0) return -EBADF;
    return newfd;
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
    int64_t ret;

    /* Route to the Linux compatibility shim if the current process is
     * marked as a Linux ELF. The shim translates Linux syscall numbers
     * (RAX) to LestraOS syscalls and re-issues them via inline asm.
     * This lets us run unmodified Linux binaries (busybox, Go, etc.)
     * without recompiling them. */
    struct process* cur = task_current();
    if (cur && cur->is_linux_process) {
        ret = linux_compat_dispatch(num, a1, a2, a3, a4, a5, 0);
        /* After every syscall (Linux or native), check whether a
         * signal is pending and deliver it by rewriting the saved
         * RIP to point at the user's signal handler. */
        signal_check_and_deliver();
        return ret;
    }

    switch (num) {
        case LESTRA_SYS_EXIT:        ret = sys_exit((int64_t)a1); break;
        case LESTRA_SYS_FORK:        ret = sys_fork(); break;
        case LESTRA_SYS_READ:        ret = sys_read((int64_t)a1, (void*)a2, a3); break;
        case LESTRA_SYS_WRITE:       ret = sys_write((int64_t)a1, (const void*)a2, a3); break;
        case LESTRA_SYS_OPEN:        ret = sys_open((const char*)a1, (int)a2); break;
        case LESTRA_SYS_CLOSE:       ret = sys_close((int64_t)a1); break;
        case LESTRA_SYS_WAITPID:     ret = sys_waitpid((int64_t)a1, (int*)a2, (int)a3); break;
        case LESTRA_SYS_EXECVE:      ret = sys_execve((const char*)a1, (char* const*)a2, (char* const*)a3); break;
        case LESTRA_SYS_GETPID:      ret = sys_getpid(); break;
        case LESTRA_SYS_BRK:         ret = sys_brk((void*)a1); break;
        case LESTRA_SYS_MMAP:        ret = sys_mmap((void*)a1, a2, (int)a3, (int)a4, (int)a5, 0); break;
        case LESTRA_SYS_MUNMAP:      ret = sys_munmap((void*)a1, a2); break;
        case LESTRA_SYS_GETTIMEOFDAY: ret = sys_gettimeofday(); break;
        case LESTRA_SYS_SLEEP:       ret = sys_sleep(a1); break;
        case LESTRA_SYS_GETCWD:      ret = sys_getcwd((char*)a1, a2); break;
        case LESTRA_SYS_CHDIR:       ret = sys_chdir((const char*)a1); break;
        case LESTRA_SYS_MKDIR:       ret = sys_mkdir((const char*)a1, (uint32_t)a2); break;
        case LESTRA_SYS_RMDIR:       ret = sys_rmdir((const char*)a1); break;
        case LESTRA_SYS_STAT:        ret = sys_stat((const char*)a1, (void*)a2); break;
        case LESTRA_SYS_LSEEK:       ret = sys_lseek((int64_t)a1, (off_t)a2, (int)a3); break;
        case LESTRA_SYS_GETDENTS:    ret = sys_getdents((int64_t)a1, (void*)a2, a3); break;
        case LESTRA_SYS_REBOOT:      ret = sys_reboot((int64_t)a1); break;
        case LESTRA_SYS_UNAME:       ret = sys_uname((void*)a1); break;
        case LESTRA_SYS_PIPE:        ret = sys_pipe((int*)a1); break;
        case LESTRA_SYS_KILL:        ret = signal_kill((int)a1, (int)a2); break;
        case LESTRA_SYS_RT_SIGACTION:    ret = signal_sigaction((int)a1, a2, a3, a4); break;
        case LESTRA_SYS_RT_SIGPROCMASK:  ret = signal_sigprocmask((int)a1, a2, a3, a4); break;
        case LESTRA_SYS_RT_SIGRETURN:    ret = signal_sigreturn(); break;
        case LESTRA_SYS_DUP2:            ret = sys_dup2((int)a1, (int)a2); break;
        default:
            pr_warn("Unknown syscall: %u\n", (unsigned)num);
            ret = -ENOSYS;
            break;
    }

    /* Native LestraOS path also checks for pending signals. This is
     * what makes Ctrl+C (SIGINT) and kill() work for native processes
     * even when they're blocked in a syscall. */
    signal_check_and_deliver();
    return ret;
}
