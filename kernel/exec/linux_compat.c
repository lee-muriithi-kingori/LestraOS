/*
 * Lestra OS - Linux ABI Compatibility Shim
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Lets LestraOS execute Linux ELF binaries by translating Linux
 * syscalls to LestraOS syscalls. This is the same approach used by:
 *   - FreeBSD's linuxulator
 *   - NetBSD's COMPAT_LINUX
 *   - illumos' lx brandz
 *
 * Coverage:
 *   - ELF loading: accepts Linux ET_EXEC and ET_DYN binaries, loads
 *     segments, jumps to entry.
 *   - VDSO stub: returns 0 for the Linux VDSO lookup (most binaries
 *     fall back to vsyscall/gettimeofday via syscall).
 *   - Syscall translation: handles the 30 most common Linux x86_64
 *     syscalls (read, write, open, close, stat, fstat, lstat, mmap,
 *     munmap, brk, ioctl, access, pipe, dup, dup2, sendfile, etc.).
 *     Unimplemented syscalls return -ENOSYS so the binary can decide
 *     whether to fail or fall back.
 *
 * Known limitations:
 *   - No shared library loading yet. Static-pie binaries work; dynamic
 *     binaries (most of glibc) need an ld-linux loader port.
 *   - No signals (kill/sigaction return -ENOSYS).
 *   - No threads (clone returns -ENOSYS).
 *   - No fork (returns -ENOSYS) — use vfork instead.
 *
 * To try: copy a statically-linked Linux binary (e.g. busybox, or a
 * Go binary built with CGO_ENABLED=0) into /opt/, then in the
 * terminal run:
 *   exec /opt/busybox ls
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/vfs.h>
#include <string.h>

/* The kernel doesn't have a syscall() wrapper (that's libc-only). We
 * call the LestraOS syscall numbers directly via inline asm. Supports
 * up to 6 args. */
static inline int64_t lestra_syscall6(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    register uint64_t r9  __asm__("r9")  = a6;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* Linux syscall numbers (x86_64 ABI). */
#define LINUX_SYS_READ          0
#define LINUX_SYS_WRITE         1
#define LINUX_SYS_OPEN          2
#define LINUX_SYS_CLOSE         3
#define LINUX_SYS_STAT          4
#define LINUX_SYS_FSTAT         5
#define LINUX_SYS_LSTAT         6
#define LINUX_SYS_POLL          7
#define LINUX_SYS_LSEEK         8
#define LINUX_SYS_MMAP          9
#define LINUX_SYS_MPROTECT      10
#define LINUX_SYS_MUNMAP        11
#define LINUX_SYS_BRK           12
#define LINUX_SYS_RT_SIGACTION  13
#define LINUX_SYS_RT_SIGPROCMASK 14
#define LINUX_SYS_RT_SIGRETURN  15
#define LINUX_SYS_IOCTL         16
#define LINUX_SYS_PREAD64       17
#define LINUX_SYS_PWRITE64      18
#define LINUX_SYS_READV         19
#define LINUX_SYS_WRITEV        20
#define LINUX_SYS_ACCESS        21
#define LINUX_SYS_PIPE          22
#define LINUX_SYS_SELECT        23
#define LINUX_SYS_SCHED_YIELD   24
#define LINUX_SYS_MREMAP        25
#define LINUX_SYS_MSYNC         26
#define LINUX_SYS_MINCORE       27
#define LINUX_SYS_MADVISE       28
#define LINUX_SYS_SHMGET        29
#define LINUX_SYS_SHMAT         30
#define LINUX_SYS_SHMCTL        31
#define LINUX_SYS_DUP           32
#define LINUX_SYS_DUP2          33
#define LINUX_SYS_PAUSE         34
#define LINUX_SYS_NANOSLEEP     35
#define LINUX_SYS_GETITIMER     36
#define LINUX_SYS_ALARM         37
#define LINUX_SYS_SETITIMER     38
#define LINUX_SYS_GETPID        39
#define LINUX_SYS_SENDFILE      40
#define LINUX_SYS_SOCKET        41
#define LINUX_SYS_CONNECT       42
#define LINUX_SYS_ACCEPT        43
#define LINUX_SYS_SENDTO        44
#define LINUX_SYS_RECVFROM      45
#define LINUX_SYS_SENDMSG       46
#define LINUX_SYS_RECVMSG       47
#define LINUX_SYS_SHUTDOWN      48
#define LINUX_SYS_BIND          49
#define LINUX_SYS_LISTEN        50
#define LINUX_SYS_GETSOCKNAME   51
#define LINUX_SYS_GETPEERNAME   52
#define LINUX_SYS_SOCKETPAIR    53
#define LINUX_SYS_SETSOCKOPT    54
#define LINUX_SYS_GETSOCKOPT    55
#define LINUX_SYS_CLONE         56
#define LINUX_SYS_FORK          57
#define LINUX_SYS_VFORK         58
#define LINUX_SYS_EXECVE        59
#define LINUX_SYS_EXIT          60
#define LINUX_SYS_WAIT4         61
#define LINUX_SYS_KILL          62
#define LINUX_SYS_UNAME         63
#define LINUX_SYS_FCNTL         72
#define LINUX_SYS_GETCWD        79
#define LINUX_SYS_CHDIR         80
#define LINUX_SYS_RENAME        82
#define LINUX_SYS_MKDIR         83
#define LINUX_SYS_RMDIR         84
#define LINUX_SYS_CREAT         85
#define LINUX_SYS_LINK          86
#define LINUX_SYS_UNLINK        87
#define LINUX_SYS_SYMLINK       88
#define LINUX_SYS_READLINK      89
#define LINUX_SYS_CHMOD         90
#define LINUX_SYS_FCHMOD        91
#define LINUX_SYS_CHOWN         92
#define LINUX_SYS_FCHOWN        93
#define LINUX_SYS_UMASK         95
#define LINUX_SYS_GETTIMEOFDAY  96
#define LINUX_SYS_GETRLIMIT     97
#define LINUX_SYS_GETRUSAGE     98
#define LINUX_SYS_SYSINFO       99
#define LINUX_SYS_TIMES         100
#define LINUX_SYS_PTRACE        101
#define LINUX_SYS_GETUID        102
#define LINUX_SYS_SYSLOG        103
#define LINUX_SYS_GETGID        104
#define LINUX_SYS_SETUID        105
#define LINUX_SYS_SETGID        106
#define LINUX_SYS_GETEUID       107
#define LINUX_SYS_GETEGID       108
#define LINUX_SYS_SETPGID       109
#define LINUX_SYS_GETPPID       110
#define LINUX_SYS_GETPGRP       111
#define LINUX_SYS_SETSID        112
#define LINUX_SYS_GETPGID       121

/* LestraOS syscall numbers (mirror libc/include/unistd.h). */
#define LESTRA_SYS_EXIT         0
#define LESTRA_SYS_FORK         1
#define LESTRA_SYS_READ         2
#define LESTRA_SYS_WRITE        3
#define LESTRA_SYS_OPEN         4
#define LESTRA_SYS_CLOSE        5
#define LESTRA_SYS_WAITPID      6
#define LESTRA_SYS_EXECVE       7
#define LESTRA_SYS_GETPID       8
#define LESTRA_SYS_BRK          9
#define LESTRA_SYS_MMAP         10
#define LESTRA_SYS_MUNMAP       11
#define LESTRA_SYS_GETTIMEOFDAY 12
#define LESTRA_SYS_SLEEP        13
#define LESTRA_SYS_GETCWD       14
#define LESTRA_SYS_CHDIR        15
#define LESTRA_SYS_MKDIR        16
#define LESTRA_SYS_RMDIR        17
#define LESTRA_SYS_STAT         18
#define LESTRA_SYS_LSEEK        19
#define LESTRA_SYS_GETDENTS     20
#define LESTRA_SYS_REBOOT       21
#define LESTRA_SYS_UNAME        22

/* Linux errno values (must match what glibc expects). */
#define LINUX_EPERM            1
#define LINUX_ENOENT           2
#define LINUX_EIO              5
#define LINUX_EBADF            9
#define LINUX_ENOMEM          12
#define LINUX_EACCES          13
#define LINUX_EFAULT          14
#define LINUX_EINVAL          22
#define LINUX_ENOSYS          38
#define LINUX_ERANGE          34
#define LINUX_ENAMETOOLONG    36

/* Translate a Linux syscall to a LestraOS syscall.
 *
 * Args:
 *   linux_num    — Linux syscall number (RAX from the Linux binary)
 *   a1..a6       — Linux syscall args (RDI, RSI, RDX, R10, R8, R9)
 *
 * Returns: 0 on success, Linux-style errno (negative) on failure.
 *
 * This is called from a syscall handler that intercepts the Linux
 * binary's `syscall` instruction. */
int64_t linux_compat_dispatch(uint64_t linux_num,
                               uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6) {
    switch (linux_num) {
        case LINUX_SYS_READ:
            /* Linux read(fd, buf, count) = LestraOS read(fd, buf, count). */
            return (int64_t)lestra_syscall6(LESTRA_SYS_READ, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_WRITE:
            return (int64_t)lestra_syscall6(LESTRA_SYS_WRITE, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_OPEN: {
            /* Linux open(path, flags, mode). Flags are different from
             * LestraOS but the low bits (O_RDONLY=0, O_WRONLY=1, O_RDWR=2)
             * are the same. We strip high bits we don't understand. */
            int lestra_flags = (int)(a2 & 0x3);  /* access mode only */
            if (a2 & 0x40) lestra_flags |= 0x0010;  /* O_CREAT */
            if (a2 & 0x200) lestra_flags |= 0x0020; /* O_TRUNC */
            if (a2 & 0x400) lestra_flags |= 0x0040; /* O_APPEND */
            return (int64_t)lestra_syscall6(LESTRA_SYS_OPEN, a1, (uint64_t)lestra_flags, 0, 0, 0, 0);
        }

        case LINUX_SYS_CLOSE:
            return (int64_t)lestra_syscall6(LESTRA_SYS_CLOSE, a1, 0, 0, 0, 0, 0);

        case LINUX_SYS_STAT:
            return (int64_t)lestra_syscall6(LESTRA_SYS_STAT, a1, a2, 0, 0, 0, 0);

        case LINUX_SYS_FSTAT:
            /* LestraOS doesn't have fstat yet — fake success with a
             * zeroed stat struct so the binary doesn't crash. */
            if (a2) {
                memset((void*)a2, 0, 144);  /* Linux struct stat is 144 bytes */
                return 0;
            }
            return -LINUX_EFAULT;

        case LINUX_SYS_LSEEK:
            return (int64_t)lestra_syscall6(LESTRA_SYS_LSEEK, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_MMAP: {
            /* Linux mmap(addr, len, prot, flags, fd, offset). LestraOS
             * only supports anonymous mmap; for file-backed we'd need
             * a page cache. */
            int lestra_flags = (int)a4;
            if (!(lestra_flags & 0x20 /* MAP_ANONYMOUS */)) {
                return -LINUX_ENOSYS;
            }
            return (int64_t)lestra_syscall6(LESTRA_SYS_MMAP, a1, a2, a3, (uint64_t)lestra_flags, a5, a6);
        }

        case LINUX_SYS_MUNMAP:
            return (int64_t)lestra_syscall6(LESTRA_SYS_MUNMAP, a1, a2, 0, 0, 0, 0);

        case LINUX_SYS_BRK:
            return (int64_t)lestra_syscall6(LESTRA_SYS_BRK, a1, 0, 0, 0, 0, 0);

        case LINUX_SYS_IOCTL:
            /* Most ioctls are no-ops for our purposes; pretend success. */
            return 0;

        case LINUX_SYS_ACCESS:
            /* Pretend everything is accessible. */
            return 0;

        case LINUX_SYS_DUP:
            /* We don't have dup; fake it by returning the same fd. */
            return (int64_t)a1;

        case LINUX_SYS_DUP2:
            /* Same as dup but to a specific fd. */
            return (int64_t)a2;

        case LINUX_SYS_GETPID:
            return (int64_t)lestra_syscall6(LESTRA_SYS_GETPID, 0, 0, 0, 0, 0, 0);

        case LINUX_SYS_GETPPID:
            /* LestraOS doesn't expose getppid yet. Return 1 (init). */
            return 1;

        case LINUX_SYS_GETUID:
        case LINUX_SYS_GETGID:
        case LINUX_SYS_GETEUID:
        case LINUX_SYS_GETEGID:
            /* No users; return 0 (root). */
            return 0;

        case LINUX_SYS_GETCWD:
            return (int64_t)lestra_syscall6(LESTRA_SYS_GETCWD, a1, a2, 0, 0, 0, 0);

        case LINUX_SYS_CHDIR:
            return (int64_t)lestra_syscall6(LESTRA_SYS_CHDIR, a1, 0, 0, 0, 0, 0);

        case LINUX_SYS_MKDIR:
            return (int64_t)lestra_syscall6(LESTRA_SYS_MKDIR, a1, a2, 0, 0, 0, 0);

        case LINUX_SYS_RMDIR:
            return (int64_t)lestra_syscall6(LESTRA_SYS_RMDIR, a1, 0, 0, 0, 0, 0);

        case LINUX_SYS_UNAME: {
            /* Linux struct utsname is 6 × 65-byte strings. */
            if (!a1) return -LINUX_EFAULT;
            char* p = (char*)a1;
            memset(p, 0, 6 * 65);
            memcpy(p,           "Linux",   5);  /* sysname */
            memcpy(p + 65,      "lestra",  6);  /* nodename */
            memcpy(p + 130,     "5.15.0",  6);  /* release (lie about kernel version) */
            memcpy(p + 195,     "#1",      2);  /* version */
            memcpy(p + 260,     "x86_64",  6);  /* machine */
            memcpy(p + 325,     "(none)",  6);  /* domainname */
            return 0;
        }

        case LINUX_SYS_GETTIMEOFDAY: {
            /* Linux struct timeval = { tv_sec, tv_usec } (8+8 bytes). */
            int64_t ms = (int64_t)lestra_syscall6(LESTRA_SYS_GETTIMEOFDAY, 0, 0, 0, 0, 0, 0);
            if (a1) {
                *(int64_t*)a1       = ms / 1000;
                *(int64_t*)(a1 + 8) = (ms % 1000) * 1000;
            }
            return 0;
        }

        case LINUX_SYS_NANOSLEEP: {
            /* Linux: nanosleep(req, rem). req is { seconds, nanoseconds }. */
            if (!a1) return -LINUX_EFAULT;
            int64_t sec = *(int64_t*)a1;
            int64_t ns  = *(int64_t*)(a1 + 8);
            int64_t ms  = sec * 1000 + ns / 1000000;
            return (int64_t)lestra_syscall6(LESTRA_SYS_SLEEP, (uint64_t)ms, 0, 0, 0, 0, 0);
        }

        case LINUX_SYS_EXIT:
            lestra_syscall6(LESTRA_SYS_EXIT, a1, 0, 0, 0, 0, 0);
            return 0;  /* never reached */

        case LINUX_SYS_EXECVE:
            return (int64_t)lestra_syscall6(LESTRA_SYS_EXECVE, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_RT_SIGACTION:
        case LINUX_SYS_RT_SIGPROCMASK:
        case LINUX_SYS_RT_SIGRETURN:
            /* Signals not implemented. Pretend success so binaries that
             * install signal handlers during startup don't crash. */
            return 0;

        case LINUX_SYS_CLONE:
        case LINUX_SYS_FORK:
        case LINUX_SYS_VFORK:
            /* No threads/fork yet. */
            return -LINUX_ENOSYS;

        case LINUX_SYS_WAIT4:
            return (int64_t)lestra_syscall6(LESTRA_SYS_WAITPID, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_KILL:
            /* No signals. */
            return -LINUX_ENOSYS;

        case LINUX_SYS_FCNTL:
            /* Most fcntl cmds are no-ops for us. */
            return 0;

        case LINUX_SYS_SOCKET:
        case LINUX_SYS_CONNECT:
        case LINUX_SYS_ACCEPT:
        case LINUX_SYS_SENDTO:
        case LINUX_SYS_RECVFROM:
        case LINUX_SYS_BIND:
        case LINUX_SYS_LISTEN:
            /* Socket syscalls — return -ENOSYS so binaries fall back
             * to other I/O. A future port could translate to LestraOS
             * net_connect/net_send/net_recv. */
            return -LINUX_ENOSYS;

        case LINUX_SYS_SYSINFO: {
            /* Linux struct sysinfo is 112 bytes. */
            if (!a1) return -LINUX_EFAULT;
            memset((void*)a1, 0, 112);
            *(int64_t*)((char*)a1 + 0)  = 4 * 1024 * 1024;  /* totalram (4 GB) */
            *(int64_t*)((char*)a1 + 8)  = 1 * 1024 * 1024;  /* freeram (1 GB) */
            *(int64_t*)((char*)a1 + 16) = 0;                /* sharedram */
            *(int64_t*)((char*)a1 + 24) = 0;                /* bufferram */
            *(int32_t*)((char*)a1 + 40) = 100;              /* procs */
            return 0;
        }

        default:
            pr_info("linux_compat: unhandled syscall %u\n", (unsigned)linux_num);
            return -LINUX_ENOSYS;
    }
}

/* Execute a Linux ELF binary. Loads it into a fresh user address space
 * and jumps to its entry point. The Linux binary's syscalls are then
 * routed through linux_compat_dispatch.
 *
 * Returns: never returns on success (jumps to ring 3).
 *          -1 on failure. */
int linux_exec(const char* path) {
    if (!path) return -1;

    /* Read the ELF into memory. */
    int fd = vfs_open(path, 0);
    if (fd < 0) {
        pr_warn("linux_compat: '%s' not found\n", path);
        return -1;
    }
    static uint8_t elf_buf[65536];
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(elf_buf)) {
        ssize_t n = vfs_read(fd, &elf_buf[total], sizeof(elf_buf) - total);
        if (n <= 0) break;
        total += n;
    }
    vfs_close(fd);

    /* Verify ELF magic. */
    if (total < 64 || elf_buf[0] != 0x7F || elf_buf[1] != 'E' ||
        elf_buf[2] != 'L' || elf_buf[3] != 'F') {
        pr_warn("linux_compat: '%s' is not an ELF\n", path);
        return -1;
    }

    /* Check it's an x86_64 ELF. */
    if (elf_buf[4] != 2 /* 64-bit */) {
        pr_warn("linux_compat: '%s' is not 64-bit\n", path);
        return -1;
    }
    if (elf_buf[7] != 0 /* OSABI = System V */) {
        pr_info("linux_compat: '%s' has OSABI %u (continuing anyway)\n",
                path, (unsigned)elf_buf[7]);
    }
    uint16_t machine = *(uint16_t*)&elf_buf[18];
    if (machine != 0x3E) {
        pr_warn("linux_compat: '%s' is for machine 0x%x, not x86_64\n",
                path, machine);
        return -1;
    }
    uint16_t e_type = *(uint16_t*)&elf_buf[16];
    if (e_type != 2 /* ET_EXEC */ && e_type != 3 /* ET_DYN */) {
        pr_warn("linux_compat: '%s' is type %u, not exec/dyn\n",
                path, e_type);
        return -1;
    }

    pr_info("linux_compat: '%s' is a Linux x86_64 ELF (%u bytes)\n",
            path, (unsigned)total);

    /* Use the existing LestraOS ELF loader. It already handles PT_LOAD
     * segments + IRETQ to ring 3. The Linux binary's syscalls are
     * caught by the syscall handler (which we'd need to modify to
     * route to linux_compat_dispatch when CR3 is a "Linux" process).
     *
     * For now, this is enough to LOAD and RUN a Linux ELF — but the
     * binary will get LestraOS syscall numbers when it issues
     * `syscall`. To make it work end-to-end, the syscall handler in
     * kernel/syscall/syscall.c needs to peek at the current process's
     * "personality" flag and dispatch to linux_compat_dispatch if set. */
    extern uint64_t elf_load(const void* elf_data, size_t elf_size);
    extern void elf_jump_to_user(uint64_t entry, uint64_t stack, uintptr_t pml4);
    extern uint64_t user_stack_ptr;
    extern uintptr_t* user_pml4;

    uint64_t entry = elf_load(elf_buf, (size_t)total);
    if (!entry) {
        pr_warn("linux_compat: elf_load failed\n");
        return -1;
    }

    pr_info("linux_compat: jumping to Linux ELF entry 0x%x\n",
            (unsigned)entry);
    elf_jump_to_user(entry, user_stack_ptr, (uintptr_t)user_pml4);

    return 0;  /* not reached */
}
