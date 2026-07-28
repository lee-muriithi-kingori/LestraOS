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
#include <lestra/pipe.h>
#include <lestra/socket.h>
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
#define ESPIPE         29   /* Illegal seek (on pipe/terminal) */
#define EMFILE         24   /* Too many open files */
#define ENOTTY         25   /* Inappropriate ioctl for device */
#define ENOTEMPTY      39   /* Directory not empty */
#define EPROTONOSUPPORT 93  /* Protocol not supported */
#define EAFNOSUPPORT    97  /* Address family not supported */
#define ENOTSOCK        88  /* Socket operation on non-socket */
#define EOPNOTSUPP     102  /* Operation not supported */
#define EADDRINUSE      98  /* Address already in use */
#define EISCONN        106  /* Transport endpoint already connected */
#define ENOTCONN       107  /* Transport endpoint not connected */
#define ECONNREFUSED   111  /* Connection refused */

/* Extra stat mode type bits not in vfs.h */
#define S_IFCHR   0020000   /* character device */
#define S_IFIFO   0010000   /* FIFO (named pipe) */

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
#define LESTRA_SYS_UNLINK           29
#define LESTRA_SYS_CHMOD            30
#define LESTRA_SYS_FSTAT            31
#define LESTRA_SYS_ACCESS           32
#define LESTRA_SYS_RENAME           33
#define LESTRA_SYS_IOCTL            34
#define LESTRA_SYS_GETUID           35
#define LESTRA_SYS_GETGID           36
#define LESTRA_SYS_GETPPID          37
#define LESTRA_SYS_SETUID           38
#define LESTRA_SYS_TIMES            39
#define LESTRA_SYS_CLOCK_GETTIME    40
#define LESTRA_SYS_GETRLIMIT        41
#define LESTRA_SYS_SETRLIMIT        42
#define LESTRA_SYS_FUTEX            43
#define LESTRA_SYS_SOCKET           44
#define LESTRA_SYS_BIND             45
#define LESTRA_SYS_CONNECT          46
#define LESTRA_SYS_LISTEN           47
#define LESTRA_SYS_ACCEPT           48
#define LESTRA_SYS_SEND             49
#define LESTRA_SYS_RECV             50
#define LESTRA_SYS_POLL             51
#define LESTRA_SYS_SELECT           52

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

/* ioctl request codes (must match Linux layout for compat). */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define FIONREAD   0x541B

/* Clock IDs for clock_gettime */
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2

/* Resource limit IDs for getrlimit/setrlimit */
#define RLIMIT_CPU       0
#define RLIMIT_FSIZE     1
#define RLIMIT_DATA      2
#define RLIMIT_STACK     3
#define RLIMIT_CORE      4
#define RLIMIT_RSS       5
#define RLIMIT_NOFILE    7
#define RLIMIT_AS        9
#define RLIMIT_NPROC     6

/* futex op codes */
#define FUTEX_WAIT      0
#define FUTEX_WAKE      1

/* poll event flags */
#define POLLIN      0x0001
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010

/* select: max fds per set (FD_SETSIZE, must be power of 2 divisible by 8) */
#define FD_SETSIZE_L  128
#define FD_SETSIZE_BYTES (FD_SETSIZE_L / 8)

/* timespec structure for clock_gettime */
struct timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* tms structure for times() */
struct tms {
    int64_t tms_utime;
    int64_t tms_stime;
    int64_t tms_cutime;
    int64_t tms_cstime;
};

/* rlimit structure for getrlimit/setrlimit */
struct rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

/* pollfd structure for poll() */
struct pollfd {
    int fd;
    short events;
    short revents;
};

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

static int64_t sys_read(int64_t fd_num, void* buf, size_t count) {
    if (!buf || count == 0) return -EFAULT;
    struct process* cur = task_current();
    if (!cur) return -EBADF;
    if (fd_num < 0 || fd_num >= MAX_FD_PER_PROC) return -EBADF;

    struct fd_entry* entry = &cur->fds[fd_num];
    if (entry->type == FD_UNUSED) return -EBADF;

    switch (entry->type) {
        case FD_SPECIAL:
            /* stdin (fd 0) reads from keyboard */
            if (fd_num == 0) {
                char* cbuf = (char*)buf;
                for (size_t i = 0; i < count; i++) {
                    cbuf[i] = keyboard_getchar();
                }
                return (int64_t)count;
            }
            /* stdout/stderr cannot be read */
            return -EBADF;

        case FD_VFS:
            /* Try positional read (memfs). Falls back to sequential
             * read for ext2/procfs/devfs that don't support offsets. */
            ssize_t n = vfs_read_at(entry->resource, buf, count, entry->offset);
            if (n >= 0) {
                entry->offset += n;
                return (int64_t)n;
            }
            /* vfs_read_at returned -1 (unsupported fd type or EOF on
             * empty file). Try vfs_read which handles ext2/procfs/devfs. */
            n = vfs_read(entry->resource, buf, count);
            return (n < 0) ? -EIO : (int64_t)n;

        case FD_PIPE:
            /* pipe: read from pipe endpoint (blocking I/O) */
            return (int64_t)pipe_read(entry->resource, buf, count);

        default:
            return -EBADF;
    }
}

static int64_t sys_write(int64_t fd_num, const void* buf, size_t count) {
    if (!buf || count == 0) return -EFAULT;
    struct process* cur = task_current();
    if (!cur) return -EBADF;
    if (fd_num < 0 || fd_num >= MAX_FD_PER_PROC) return -EBADF;

    struct fd_entry* entry = &cur->fds[fd_num];
    if (entry->type == FD_UNUSED) return -EBADF;

    switch (entry->type) {
        case FD_SPECIAL:
            /* stdout/stderr go to VGA+serial */
            if (fd_num == 1 || fd_num == 2) {
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
            /* stdin cannot be written */
            return -EBADF;

        case FD_VFS:
            /* O_APPEND: if the fd was opened with O_APPEND, seek to
             * end before writing (POSIX semantics). For memfs files we
             * can use vfs_lseek to get the file size. */
            if (entry->flags & O_APPEND) {
                off_t end = vfs_lseek(entry->resource, 0, 2);  /* SEEK_END */
                if (end >= 0) entry->offset = end;
            }
            /* Try positional write (memfs). Falls back to sequential
             * write for ext2/procfs/devfs that don't support offsets. */
            ssize_t n = vfs_write_at(entry->resource, buf, count, entry->offset);
            if (n >= 0) {
                entry->offset += n;
                return (int64_t)n;
            }
            /* vfs_write_at returned -1 (unsupported fd type). Try
             * vfs_write which handles ext2/procfs/devfs. */
            n = vfs_write(entry->resource, buf, count);
            return (n < 0) ? -EIO : (int64_t)n;

        case FD_PIPE:
            /* pipe: write to pipe endpoint (blocking I/O) */
            return (int64_t)pipe_write(entry->resource, buf, count);

        default:
            return -EBADF;
    }
}

/* Per-process CWD. Single static buffer for now. */
static char cwd[MAX_PATH_LEN] = "/";

static int64_t sys_open(const char* path, int flags) {
    if (!path) return -EFAULT;
    struct process* cur = task_current();
    if (!cur) return -EFAULT;

    /* Resolve relative paths against the per-process CWD.
     * If the path doesn't start with '/', prepend the CWD. */
    char resolved[MAX_PATH_LEN];
    if (path[0] != '/') {
        /* Relative path: prepend cwd */
        size_t cwd_len = strlen(cwd);
        size_t path_len = strlen(path);
        /* Handle "./" prefix — just strip it */
        if (path[0] == '.' && (path[1] == '/' || path[1] == '\0')) {
            path += 1;
            if (*path == '/') path++;
            path_len = strlen(path);
        }
        if (cwd_len + 1 + path_len >= MAX_PATH_LEN) return -ENAMETOOLONG;
        memcpy(resolved, cwd, cwd_len);
        if (cwd_len > 0 && cwd[cwd_len - 1] != '/') {
            resolved[cwd_len] = '/';
            cwd_len++;
        }
        memcpy(resolved + cwd_len, path, path_len + 1);
        path = resolved;
    }

    /* Find a free fd slot (starting from 3, since 0-2 are reserved) */
    int local_fd = -1;
    for (int i = 3; i < MAX_FD_PER_PROC; i++) {
        if (cur->fds[i].type == FD_UNUSED) {
            local_fd = i;
            break;
        }
    }
    if (local_fd < 0) return -EMFILE;

    /* Open the file in VFS to get a global VFS fd */
    int vfs_fd = vfs_open(path, flags);
    if (vfs_fd < 0) return -ENOENT;

    /* Set up the per-process fd entry */
    cur->fds[local_fd].type = FD_VFS;
    cur->fds[local_fd].resource = vfs_fd;
    cur->fds[local_fd].offset = 0;
    cur->fds[local_fd].flags = flags;

    return (int64_t)local_fd;
}

static int64_t sys_close(int64_t fd_num) {
    struct process* cur = task_current();
    if (!cur) return -EBADF;
    if (fd_num < 0 || fd_num >= MAX_FD_PER_PROC) return -EBADF;

    struct fd_entry* entry = &cur->fds[fd_num];
    if (entry->type == FD_UNUSED) return -EBADF;

    /* Close the underlying resource */
    if (entry->type == FD_VFS) {
        vfs_close(entry->resource);
    } else if (entry->type == FD_PIPE) {
        pipe_close(entry->resource);
    }
    /* FD_SPECIAL (stdin/stdout/stderr) has no underlying resource to close */

    /* Mark fd as unused */
    entry->type = FD_UNUSED;
    entry->resource = 0;
    entry->offset = 0;
    entry->flags = 0;

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
    int rc = vfs_mkdir(path, mode ? (mode & 0777) : 0755);
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

static int64_t sys_lseek(int64_t fd_num, off_t offset, int whence) {
    struct process* cur = task_current();
    if (!cur) return -EBADF;
    if (fd_num < 0 || fd_num >= MAX_FD_PER_PROC) return -EBADF;

    struct fd_entry* entry = &cur->fds[fd_num];
    if (entry->type == FD_UNUSED) return -EBADF;

    /* Pipes and special fds don't support seeking */
    if (entry->type == FD_SPECIAL || entry->type == FD_PIPE) return -ESPIPE;

    /* For VFS files, compute new offset from per-process entry */
    off_t base;
    switch (whence) {
        case 0: base = 0; break;                /* SEEK_SET */
        case 1: base = entry->offset; break;    /* SEEK_CUR */
        case 2: /* SEEK_END — need file size from VFS */
            base = vfs_lseek(entry->resource, 0, 2);
            if (base < 0) return -EINVAL;
            break;
        default: return -EINVAL;
    }

    off_t new_off = base + offset;
    if (new_off < 0) return -EINVAL;
    entry->offset = new_off;
    return (int64_t)new_off;
}

static int64_t sys_unlink(const char* path) {
    if (!path) return -EFAULT;
    int rc = vfs_unlink(path);
    return (rc < 0) ? -ENOENT : 0;
}

static int64_t sys_getdents(int64_t fd_num, void* dirp, size_t count) {
    /* VFS exposes vfs_readdir one entry at a time. We pack as many
     * struct dirent entries as fit in the user buffer.
     *
     * With the per-process fd table, the local fd must be mapped to
     * the VFS resource via cur->fds[fd_num].resource. */
    if (!dirp || count == 0) return -EINVAL;
    struct process* cur = task_current();
    if (!cur) return -EBADF;
    if (fd_num < 0 || fd_num >= MAX_FD_PER_PROC) return -EBADF;

    struct fd_entry* entry = &cur->fds[fd_num];
    if (entry->type == FD_UNUSED) return -EBADF;
    if (entry->type != FD_VFS) return -EINVAL;  /* only VFS fds support getdents */

    int vfs_fd = entry->resource;
    struct dirent* out = (struct dirent*)dirp;
    size_t bytes = 0;
    int idx = 0;
    while (bytes + sizeof(struct dirent) <= count) {
        int rc = vfs_readdir(vfs_fd, &out[idx]);
        /* vfs_readdir returns 0 on success, -1 on end-of-directory.
         * The old code used "rc <= 0" which treated success (0) as
         * end-of-directory — that was a bug. Fixed to "rc < 0". */
        if (rc < 0) break;
        idx++;
        bytes += sizeof(struct dirent);
    }
    return (int64_t)bytes;
}

static int64_t sys_reboot(int64_t cmd) {
    if (cmd == 0) {
        printk("Shutting down...\n");
        extern void shutdown_system(void);
        shutdown_system();
    } else {
        printk("Rebooting...\n");
        extern void reboot_system(void);
        reboot_system();
    }
    /* Should not reach here — shutdown/reboot should halt or reset. */
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
    struct process* cur = task_current();
    if (!cur) return -EFAULT;

    /* Find two free fd slots for the pipe read and write ends */
    int fd_read = -1, fd_write = -1;
    for (int i = 0; i < MAX_FD_PER_PROC; i++) {
        if (cur->fds[i].type == FD_UNUSED) {
            if (fd_read < 0) fd_read = i;
            else if (fd_write < 0) { fd_write = i; break; }
        }
    }
    if (fd_read < 0 || fd_write < 0) return -EMFILE;

    /* Create the pipe (returns global pipe fds in pipe_fds[2]) */
    int pipe_fds[2];
    int rc = pipe_create(pipe_fds);
    if (rc < 0) return -1;

    /* Set up per-process fd entries */
    cur->fds[fd_read].type = FD_PIPE;
    cur->fds[fd_read].resource = pipe_fds[0];  /* read end */
    cur->fds[fd_read].offset = 0;
    cur->fds[fd_read].flags = O_RDONLY;

    cur->fds[fd_write].type = FD_PIPE;
    cur->fds[fd_write].resource = pipe_fds[1];  /* write end */
    cur->fds[fd_write].offset = 0;
    cur->fds[fd_write].flags = O_WRONLY;

    user_fds[0] = fd_read;
    user_fds[1] = fd_write;
    return 0;
}

static int64_t sys_dup2(int oldfd, int newfd) {
    struct process* cur = task_current();
    if (!cur) return -EBADF;
    if (oldfd < 0 || oldfd >= MAX_FD_PER_PROC) return -EBADF;
    if (newfd < 0 || newfd >= MAX_FD_PER_PROC) return -EBADF;
    if (oldfd == newfd) {
        /* POSIX: if oldfd is valid, return newfd without closing it */
        if (cur->fds[oldfd].type == FD_UNUSED) return -EBADF;
        return (int64_t)newfd;
    }

    struct fd_entry* old_entry = &cur->fds[oldfd];
    if (old_entry->type == FD_UNUSED) return -EBADF;

    /* Close newfd if it's currently open */
    if (cur->fds[newfd].type != FD_UNUSED) {
        if (cur->fds[newfd].type == FD_VFS) {
            vfs_close(cur->fds[newfd].resource);
        } else if (cur->fds[newfd].type == FD_PIPE) {
            pipe_close(cur->fds[newfd].resource);
        }
    }

    /* Copy oldfd's fd entry to newfd (both now share the same resource) */
    cur->fds[newfd] = *old_entry;

    return (int64_t)newfd;
}

/* ── New syscalls (30–52) ─────────────────────────────────────── */

static int64_t sys_chmod(const char* path, uint32_t mode) {
    if (!path) return -EFAULT;
    /* VFS doesn't have a chmod operation yet. Return -ENOSYS so
     * callers know the feature isn't implemented rather than
     * silently succeeding with no effect. */
    (void)mode;
    return -ENOSYS;
}

static int64_t sys_fstat(int64_t fd_num, void* st) {
    if (!st) return -EFAULT;
    struct process* cur = task_current();
    if (!cur) return -EBADF;
    if (fd_num < 0 || fd_num >= MAX_FD_PER_PROC) return -EBADF;

    struct fd_entry* entry = &cur->fds[fd_num];
    if (entry->type == FD_UNUSED) return -EBADF;

    /* For VFS fds, stat the underlying VFS resource by fd number.
     * We don't have vfs_fstat(), so we construct a stat from what
     * we know (offset = file position, no real inode data). */
    struct stat* s = (struct stat*)st;
    memset(s, 0, sizeof(*s));

    if (entry->type == FD_SPECIAL) {
        /* stdin/stdout/stderr — treat as character device */
        s->mode = S_IFCHR | 0666;
        s->uid = 0;
        s->gid = 0;
        s->size = 0;
        return 0;
    }

    if (entry->type == FD_VFS) {
        /* Try to get stat via vfs_stat on the open resource.
         * Since vfs_stat takes a path and we only have a fd,
         * we fill in what we can. For memfs files, lseek
         * SEEK_END gives the file size. */
        off_t end = vfs_lseek(entry->resource, 0, 2);
        if (end >= 0) {
            s->mode = S_IFREG | 0644;
            s->size = (uint64_t)end;
        } else {
            /* Unsupported VFS fd type (ext2, procfs, devfs) —
             * provide a generic stat. */
            s->mode = S_IFREG | 0644;
            s->size = 0;
        }
        s->uid = 0;
        s->gid = 0;
        s->atime = timer_get_ms();
        s->mtime = timer_get_ms();
        s->ctime = timer_get_ms();
        return 0;
    }

    if (entry->type == FD_PIPE) {
        s->mode = S_IFIFO | 0666;
        s->uid = 0;
        s->gid = 0;
        s->size = 0;
        return 0;
    }

    return -EBADF;
}

static int64_t sys_access(const char* path, int mode) {
    if (!path) return -EFAULT;
    /* access() checks whether the calling process can access a file.
     * In our single-user root system, any existing file is accessible.
     * Check that the file exists via VFS lookup; if it does, return 0.
     * mode bits (R_OK, W_OK, X_OK) are ignored since we're root. */
    (void)mode;
    /* Resolve relative path against cwd like sys_open does. */
    char resolved[MAX_PATH_LEN];
    const char* check_path = path;
    if (path[0] != '/') {
        size_t cwd_len = strlen(cwd);
        size_t path_len = strlen(path);
        if (cwd_len + 1 + path_len >= MAX_PATH_LEN) return -ENAMETOOLONG;
        memcpy(resolved, cwd, cwd_len);
        if (cwd_len > 0 && cwd[cwd_len - 1] != '/') {
            resolved[cwd_len] = '/';
            cwd_len++;
        }
        memcpy(resolved + cwd_len, path, path_len + 1);
        check_path = resolved;
    }
    struct vnode* vn = vfs_lookup(check_path);
    if (!vn) return -ENOENT;
    return 0;
}

static int64_t sys_rename(const char* oldpath, const char* newpath) {
    if (!oldpath || !newpath) return -EFAULT;
    /* VFS doesn't support rename yet. Return -ENOSYS. */
    return -ENOSYS;
}

static int64_t sys_ioctl(int64_t fd_num, uint64_t request, uint64_t arg) {
    struct process* cur = task_current();
    if (!cur) return -EBADF;
    if (fd_num < 0 || fd_num >= MAX_FD_PER_PROC) return -EBADF;

    struct fd_entry* entry = &cur->fds[fd_num];
    if (entry->type == FD_UNUSED) return -EBADF;

    /* Socket fds — delegate to socket layer */
    if (socket_is_socket_fd(fd_num)) {
        /* Sockets don't support ioctl; return -ENOTTY */
        return -ENOTTY;
    }

    switch (request) {
        case TCGETS:
        case TCSETS:
            /* Terminal get/set attributes. For FD_SPECIAL (stdin/stdout)
             * and any tty-like fd, just return success. arg points to
             * a struct termios in userspace; we accept it silently. */
            if (entry->type == FD_SPECIAL) return 0;
            /* Pipes can also be treated as tty-like for ioctl */
            if (entry->type == FD_PIPE) return 0;
            return -ENOTTY;

        case FIONREAD:
            /* Number of bytes readable. For stdin (fd 0), check the
             * keyboard buffer. For other fds, return 0. */
            if (entry->type == FD_SPECIAL && fd_num == 0) {
                int count = keyboard_has_key() ? 1 : 0;
                if (arg) *(int*)(uintptr_t)arg = count;
                return 0;
            }
            if (entry->type == FD_PIPE) {
                /* We don't have a pipe_readable_count helper yet;
                 * return 0 as a safe default. */
                if (arg) *(int*)(uintptr_t)arg = 0;
                return 0;
            }
            if (entry->type == FD_VFS) {
                /* For VFS files, readable = file_size - current_offset */
                off_t end = vfs_lseek(entry->resource, 0, 2);
                off_t readable = (end >= 0 && end > entry->offset)
                    ? end - entry->offset : 0;
                if (arg) *(int*)(uintptr_t)arg = (int)readable;
                return 0;
            }
            return -ENOTTY;

        default:
            return -ENOTTY;
    }
}

static int64_t sys_getuid(void) {
    /* Single-user system: we are root (uid 0). */
    return 0;
}

static int64_t sys_getgid(void) {
    /* Single-user system: we are root (gid 0). */
    return 0;
}

static int64_t sys_getppid(void) {
    struct process* cur = task_current();
    if (!cur) return 0;
    return (int64_t)cur->parent_pid;
}

static int64_t sys_setuid(uint32_t uid) {
    /* Only root (uid 0) can setuid. If caller tries to set uid to
     * anything non-zero, return -EPERM. Setting to 0 is fine. */
    if (uid != 0) return -EPERM;
    return 0;
}

static int64_t sys_times(void* buf) {
    /* Return elapsed ticks in milliseconds. Fill in tms struct if
     * the caller provided a buffer. Since we don't track per-process
     * CPU time separately, all fields are approximate. */
    struct tms* t = (struct tms*)buf;
    if (t) {
        uint64_t ms = timer_get_ms();
        t->tms_utime  = (int64_t)ms;   /* user time ≈ wall clock */
        t->tms_stime  = 0;              /* kernel time not tracked */
        t->tms_cutime = 0;              /* children not tracked */
        t->tms_cstime = 0;
    }
    return (int64_t)timer_get_ms();
}

static int64_t sys_clock_gettime(int clk_id, void* tp) {
    if (!tp) return -EFAULT;
    struct timespec64* ts = (struct timespec64*)tp;
    uint64_t ms = timer_get_ms();

    switch (clk_id) {
        case CLOCK_REALTIME:
            /* Real-time clock: ms since boot (no RTC yet). */
            ts->tv_sec  = (int64_t)(ms / 1000);
            ts->tv_nsec = (int64_t)((ms % 1000) * 1000000);
            return 0;

        case CLOCK_MONOTONIC:
            /* Monotonic clock: same as realtime for now (no RTC). */
            ts->tv_sec  = (int64_t)(ms / 1000);
            ts->tv_nsec = (int64_t)((ms % 1000) * 1000000);
            return 0;

        case CLOCK_PROCESS_CPUTIME_ID:
            /* Per-process CPU time: approximate with wall clock. */
            ts->tv_sec  = (int64_t)(ms / 1000);
            ts->tv_nsec = (int64_t)((ms % 1000) * 1000000);
            return 0;

        default:
            return -EINVAL;
    }
}

static int64_t sys_getrlimit(int resource, void* rlim_ptr) {
    if (!rlim_ptr) return -EFAULT;
    struct rlimit* rl = (struct rlimit*)rlim_ptr;

    /* Return sensible defaults for each resource limit. In our
     * minimal OS, most limits are essentially unlimited. */
    switch (resource) {
        case RLIMIT_NOFILE:
            rl->rlim_cur = MAX_FD_PER_PROC;
            rl->rlim_max = MAX_FD_PER_PROC;
            return 0;
        case RLIMIT_STACK:
            rl->rlim_cur = 8 * 1024 * 1024;   /* 8 MB */
            rl->rlim_max = 8 * 1024 * 1024;
            return 0;
        case RLIMIT_DATA:
            rl->rlim_cur = 16 * 1024 * 1024;  /* 16 MB (matches brk cap) */
            rl->rlim_max = 16 * 1024 * 1024;
            return 0;
        case RLIMIT_AS:
            rl->rlim_cur = 256 * 1024 * 1024; /* 256 MB */
            rl->rlim_max = 256 * 1024 * 1024;
            return 0;
        case RLIMIT_CPU:
            rl->rlim_cur = 0xFFFFFFFF;         /* unlimited */
            rl->rlim_max = 0xFFFFFFFF;
            return 0;
        case RLIMIT_FSIZE:
            rl->rlim_cur = 0xFFFFFFFF;
            rl->rlim_max = 0xFFFFFFFF;
            return 0;
        case RLIMIT_CORE:
            rl->rlim_cur = 0;                  /* no core dumps */
            rl->rlim_max = 0;
            return 0;
        case RLIMIT_RSS:
            rl->rlim_cur = 0xFFFFFFFF;
            rl->rlim_max = 0xFFFFFFFF;
            return 0;
        case RLIMIT_NPROC:
            rl->rlim_cur = MAX_PROCS;
            rl->rlim_max = MAX_PROCS;
            return 0;
        default:
            return -EINVAL;
    }
}

static int64_t sys_setrlimit(int resource, const void* rlim_ptr) {
    if (!rlim_ptr) return -EFAULT;
    (void)resource;
    /* We don't allow changing resource limits yet. Return -EPERM
     * to indicate the operation is not permitted. */
    return -EPERM;
}

static int64_t sys_futex(uint32_t* uaddr, int op, uint32_t val,
                          uint64_t timeout, uint32_t* uaddr2, uint32_t val3) {
    (void)uaddr2; (void)val3; (void)timeout;
    if (!uaddr) return -EFAULT;

    switch (op) {
        case FUTEX_WAIT:
            /* Wait until *uaddr == val. In our single-threaded kernel
             * we can't truly block on a userspace word changing, so we
             * check once and either return 0 (match) or -EAGAIN (mismatch).
             * A real implementation would sleep and schedule, but for now
             * this basic stub is sufficient for simple futex usage. */
            if (*uaddr != val) return -EAGAIN;
            /* Value matches — nothing to wait for, return success. */
            return 0;

        case FUTEX_WAKE:
            /* Wake up to 'val' waiters on this futex. Since we don't
             * have a wait queue, just return 0 (no waiters woken). */
            return 0;

        default:
            return -ENOSYS;
    }
}

static int64_t sys_socket(int domain, int type, int protocol) {
    return (int64_t)socket_create(domain, type, protocol);
}

static int64_t sys_bind(int64_t fd, const void* addr, int addrlen) {
    return (int64_t)socket_bind((int)fd, (const struct sockaddr*)addr, addrlen);
}

static int64_t sys_connect(int64_t fd, const void* addr, int addrlen) {
    return (int64_t)socket_connect((int)fd, (const struct sockaddr*)addr, addrlen);
}

static int64_t sys_listen(int64_t fd, int backlog) {
    return (int64_t)socket_listen((int)fd, backlog);
}

static int64_t sys_accept(int64_t fd, void* addr, void* addrlen) {
    return (int64_t)socket_accept((int)fd, (struct sockaddr*)addr, (int*)addrlen);
}

static int64_t sys_send(int64_t fd, const void* buf, size_t len, int flags) {
    return (int64_t)socket_sendto((int)fd, buf, len, flags, NULL, 0);
}

static int64_t sys_recv(int64_t fd, void* buf, size_t len, int flags) {
    return (int64_t)socket_recvfrom((int)fd, buf, len, flags, NULL, NULL);
}

static int64_t sys_poll(void* fds_ptr, uint64_t nfds, int64_t timeout_ms) {
    struct pollfd* pfds = (struct pollfd*)fds_ptr;
    if (!pfds && nfds > 0) return -EFAULT;

    /* Basic poll: check each fd for readability/writability.
     * This is a synchronous check — we don't block since we have
     * no async I/O notification yet. If timeout > 0, we sleep
     * briefly then re-check once. */
    int ready = 0;
    struct process* cur = task_current();

    for (uint64_t i = 0; i < nfds; i++) {
        pfds[i].revents = 0;

        if (pfds[i].fd < 0) {
            /* Negative fd: ignore (revents stays 0) */
            continue;
        }

        /* Check socket fds */
        if (socket_is_socket_fd(pfds[i].fd)) {
            /* Socket is always writable if connected, readable
             * if data is available. We assume both for now. */
            if (pfds[i].events & POLLIN)  pfds[i].revents |= POLLIN;
            if (pfds[i].events & POLLOUT) pfds[i].revents |= POLLOUT;
            if (pfds[i].revents) ready++;
            continue;
        }

        /* Check local process fds */
        if (cur && pfds[i].fd >= 0 && pfds[i].fd < MAX_FD_PER_PROC) {
            struct fd_entry* entry = &cur->fds[pfds[i].fd];
            if (entry->type == FD_UNUSED) {
                pfds[i].revents |= POLLERR;
                ready++;
                continue;
            }

            if (entry->type == FD_SPECIAL) {
                /* stdin: check keyboard buffer */
                if (pfds[i].fd == 0 && (pfds[i].events & POLLIN)) {
                    if (keyboard_has_key()) {
                        pfds[i].revents |= POLLIN;
                    }
                }
                /* stdout/stderr: always writable */
                if ((pfds[i].fd == 1 || pfds[i].fd == 2)
                    && (pfds[i].events & POLLOUT)) {
                    pfds[i].revents |= POLLOUT;
                }
            } else {
                /* VFS/pipe fds: assume readable+writable for now */
                if (pfds[i].events & POLLIN)  pfds[i].revents |= POLLIN;
                if (pfds[i].events & POLLOUT) pfds[i].revents |= POLLOUT;
            }

            if (pfds[i].revents) ready++;
        }
    }

    /* If nothing is ready and timeout > 0, sleep briefly and
     * re-check once. This gives a crude approximation of blocking
     * poll behavior. */
    if (ready == 0 && timeout_ms > 0) {
        if (timeout_ms > 100) timeout_ms = 100; /* cap at 100ms */
        task_sleep((uint64_t)timeout_ms);
        /* Re-scan after waking */
        for (uint64_t i = 0; i < nfds; i++) {
            if (pfds[i].fd < 0) continue;
            if (socket_is_socket_fd(pfds[i].fd)) {
                if (pfds[i].events & POLLIN)  pfds[i].revents |= POLLIN;
                if (pfds[i].events & POLLOUT) pfds[i].revents |= POLLOUT;
                if (pfds[i].revents) ready++;
                continue;
            }
            if (cur && pfds[i].fd >= 0 && pfds[i].fd < MAX_FD_PER_PROC) {
                struct fd_entry* entry = &cur->fds[pfds[i].fd];
                if (entry->type == FD_UNUSED) {
                    if (!(pfds[i].revents & POLLERR)) {
                        pfds[i].revents |= POLLERR;
                        ready++;
                    }
                    continue;
                }
                if (entry->type == FD_SPECIAL && pfds[i].fd == 0
                    && (pfds[i].events & POLLIN)) {
                    if (keyboard_has_key() && !(pfds[i].revents & POLLIN)) {
                        pfds[i].revents |= POLLIN;
                        ready++;
                    }
                }
            }
        }
    }

    return (int64_t)ready;
}

static int64_t sys_select(int nfds, void* readfds, void* writefds,
                           void* exceptfds, const void* timeout) {
    (void)timeout;
    if (nfds < 0) return -EINVAL;
    if (nfds > FD_SETSIZE_L) nfds = FD_SETSIZE_L;

    /* select() uses bit-sets (fd_set). Each fd_set is an array of
     * longs where bit N corresponds to fd N. FD_SETSIZE_BYTES is
     * the size in bytes. */
    uint8_t* rset = (uint8_t*)readfds;
    uint8_t* wset = (uint8_t*)writefds;
    uint8_t* eset = (uint8_t*)exceptfds;
    struct process* cur = task_current();
    int ready = 0;

    for (int fd = 0; fd < nfds; fd++) {
        int fd_byte = fd / 8;
        int fd_bit  = fd % 8;
        int watching_r = rset && (rset[fd_byte] & (1 << fd_bit));
        int watching_w = wset && (wset[fd_byte] & (1 << fd_bit));
        int watching_e = eset && (eset[fd_byte] & (1 << fd_bit));

        if (!watching_r && !watching_w && !watching_e) continue;

        /* Clear the bits first (we'll set them if ready) */
        if (rset) rset[fd_byte] &= ~(1 << fd_bit);
        if (wset) wset[fd_byte] &= ~(1 << fd_bit);
        if (eset) eset[fd_byte] &= ~(1 << fd_bit);

        /* Check socket fds */
        if (socket_is_socket_fd(fd)) {
            if (watching_r) { rset[fd_byte] |= (1 << fd_bit); ready++; }
            if (watching_w) { wset[fd_byte] |= (1 << fd_bit); ready++; }
            continue;
        }

        /* Check local process fds */
        if (cur && fd >= 0 && fd < MAX_FD_PER_PROC) {
            struct fd_entry* entry = &cur->fds[fd];
            if (entry->type == FD_UNUSED) {
                if (watching_e) { eset[fd_byte] |= (1 << fd_bit); ready++; }
                continue;
            }

            if (entry->type == FD_SPECIAL) {
                if (fd == 0 && watching_r) {
                    if (keyboard_has_key()) {
                        rset[fd_byte] |= (1 << fd_bit);
                        ready++;
                    }
                }
                if ((fd == 1 || fd == 2) && watching_w) {
                    wset[fd_byte] |= (1 << fd_bit);
                    ready++;
                }
            } else {
                /* VFS/pipe: assume readable+writable */
                if (watching_r) { rset[fd_byte] |= (1 << fd_bit); ready++; }
                if (watching_w) { wset[fd_byte] |= (1 << fd_bit); ready++; }
            }
        }
    }

    return (int64_t)ready;
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
                         uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    int64_t ret;

    /* Route to the Linux compatibility shim if the current process is
     * marked as a Linux ELF. The shim translates Linux syscall numbers
     * (RAX) to LestraOS syscalls and re-issues them via inline asm.
     * This lets us run unmodified Linux binaries (busybox, Go, etc.)
     * without recompiling them. */
    struct process* cur = task_current();
    if (cur && cur->is_linux_process) {
        ret = linux_compat_dispatch(num, a1, a2, a3, a4, a5, a6);
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
        case LESTRA_SYS_MMAP:        ret = sys_mmap((void*)a1, a2, (int)a3, (int)a4, (int)a5, (off_t)a6); break;
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
        case LESTRA_SYS_UNLINK:       ret = sys_unlink((const char*)a1); break;
        case LESTRA_SYS_CHMOD:        ret = sys_chmod((const char*)a1, (uint32_t)a2); break;
        case LESTRA_SYS_FSTAT:        ret = sys_fstat((int64_t)a1, (void*)a2); break;
        case LESTRA_SYS_ACCESS:       ret = sys_access((const char*)a1, (int)a2); break;
        case LESTRA_SYS_RENAME:       ret = sys_rename((const char*)a1, (const char*)a2); break;
        case LESTRA_SYS_IOCTL:        ret = sys_ioctl((int64_t)a1, a2, a3); break;
        case LESTRA_SYS_GETUID:       ret = sys_getuid(); break;
        case LESTRA_SYS_GETGID:       ret = sys_getgid(); break;
        case LESTRA_SYS_GETPPID:      ret = sys_getppid(); break;
        case LESTRA_SYS_SETUID:       ret = sys_setuid((uint32_t)a1); break;
        case LESTRA_SYS_TIMES:        ret = sys_times((void*)a1); break;
        case LESTRA_SYS_CLOCK_GETTIME: ret = sys_clock_gettime((int)a1, (void*)a2); break;
        case LESTRA_SYS_GETRLIMIT:    ret = sys_getrlimit((int)a1, (void*)a2); break;
        case LESTRA_SYS_SETRLIMIT:    ret = sys_setrlimit((int)a1, (const void*)a2); break;
        case LESTRA_SYS_FUTEX:        ret = sys_futex((uint32_t*)a1, (int)a2, (uint32_t)a3, a4, (uint32_t*)a5, (uint32_t)a6); break;
        case LESTRA_SYS_SOCKET:       ret = sys_socket((int)a1, (int)a2, (int)a3); break;
        case LESTRA_SYS_BIND:         ret = sys_bind((int64_t)a1, (const void*)a2, (int)a3); break;
        case LESTRA_SYS_CONNECT:      ret = sys_connect((int64_t)a1, (const void*)a2, (int)a3); break;
        case LESTRA_SYS_LISTEN:       ret = sys_listen((int64_t)a1, (int)a2); break;
        case LESTRA_SYS_ACCEPT:       ret = sys_accept((int64_t)a1, (void*)a2, (void*)a3); break;
        case LESTRA_SYS_SEND:         ret = sys_send((int64_t)a1, (const void*)a2, a3, (int)a4); break;
        case LESTRA_SYS_RECV:         ret = sys_recv((int64_t)a1, (void*)a2, a3, (int)a4); break;
        case LESTRA_SYS_POLL:         ret = sys_poll((void*)a1, a2, (int64_t)a3); break;
        case LESTRA_SYS_SELECT:       ret = sys_select((int)a1, (void*)a2, (void*)a3, (void*)a4, (const void*)a5); break;
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
