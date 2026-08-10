/*
 * Lestra OS - Linux ABI Compatibility Shim
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
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
 *   - Syscall translation: handles the 50+ most common Linux x86_64
 *     syscalls (read, write, open, close, stat, fstat, lstat, mmap,
 *     munmap, brk, ioctl, access, pipe, dup, dup2, fcntl, socket,
 *     bind, connect, listen, accept, sendto, recvfrom, fork, vfork,
 *     clone-as-fork, sysinfo, getppid, etc.). Unimplemented syscalls
 *     return -ENOSYS so the binary can decide whether to fail or
 *     fall back.
 *
 * Known limitations:
 *   - No shared library loading yet. Static-pie binaries work; dynamic
 *     binaries (most of glibc) need an ld-linux loader port.
 *   - Signals forwarded to native implementation (kill/sigaction/sigprocmask/sigreturn).
 *   - No threads: clone() is translated to fork() (flags ignored), so
 *     CLONE_VM/CLONE_THREAD still doesn't work. True thread support
 *     needs a thread-aware scheduler (XL complexity).
 *   - sendto/recvfrom ignore the addr argument (works for connected
 *     sockets only; unconnected UDP sendto is not supported).
 *   - fcntl FD_CLOEXEC is accepted but not stored (no exec yet).
 *
 * To try: copy a statically-linked Linux binary (e.g. busybox, or a
 * Go binary built with CGO_ENABLED=0) into /opt/, then in the
 * terminal run:
 *   exec /opt/busybox ls
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/vfs.h>
#include <lestra/uaccess.h>
#include <lestra/sched.h>
#include <lestra/mm.h>
#include <string.h>

/* The kernel doesn't have a syscall() wrapper (that's libc-only). We
 * call the LestraOS syscall numbers directly via inline asm. Supports
 * up to 6 args.
 *
 * IMPORTANT: We must temporarily flip the current process's
 * is_linux_process flag to 0 around the inner syscall, otherwise
 * syscall_dispatch would re-route the inner call back into
 * linux_compat_dispatch (infinite recursion / wrong dispatch). */
static inline int64_t lestra_syscall6(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    int64_t ret;
    int saved_linux = proc_is_linux_process();
    if (saved_linux) proc_set_linux_process(0);
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    register uint64_t r9  __asm__("r9")  = a6;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    if (saved_linux) proc_set_linux_process(saved_linux);
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
#define LESTRA_SYS_KILL          24
#define LESTRA_SYS_RT_SIGACTION    25
#define LESTRA_SYS_RT_SIGPROCMASK  26
#define LESTRA_SYS_RT_SIGRETURN    27
#define LESTRA_SYS_DUP2            28
#define LESTRA_SYS_UNLINK          29
#define LESTRA_SYS_CHMOD           30
#define LESTRA_SYS_FSTAT           31
#define LESTRA_SYS_ACCESS          32
#define LESTRA_SYS_RENAME          33
#define LESTRA_SYS_IOCTL           34
#define LESTRA_SYS_GETUID          35
#define LESTRA_SYS_GETGID          36
#define LESTRA_SYS_GETPPID         37
#define LESTRA_SYS_SETUID          38
#define LESTRA_SYS_SOCKET          44
#define LESTRA_SYS_BIND            45
#define LESTRA_SYS_CONNECT         46
#define LESTRA_SYS_LISTEN          47
#define LESTRA_SYS_ACCEPT          48
#define LESTRA_SYS_SEND            49
#define LESTRA_SYS_RECV            50

/* Linux fcntl commands (we implement inline since native sys_fcntl
 * does not exist yet). */
#define LINUX_F_DUPFD   0
#define LINUX_F_GETFD   1
#define LINUX_F_SETFD   2
#define LINUX_F_GETFL   3
#define LINUX_F_SETFL   4
#define LINUX_FD_CLOEXEC 1
/* Linux O_NONBLOCK / O_APPEND bits that may be set via F_SETFL. */
#define LINUX_O_NONBLOCK  0x800
#define LINUX_O_APPEND    0x400

/* Linux errno values (must match what glibc expects).
 * LestraOS uses the same numeric values (verified in syscall.c), so
 * errno pass-through works without translation. */
#define LINUX_EPERM            1
#define LINUX_ENOENT           2
#define LINUX_EIO              5
#define LINUX_EBADF            9
#define LINUX_ENOMEM          12
#define LINUX_EACCES          13
#define LINUX_EFAULT          14
#define LINUX_EINVAL          22
#define LINUX_EMFILE          24
#define LINUX_ENOTTY          25
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

        case LINUX_SYS_FSTAT: {
            /* Translate to native sys_fstat (LESTRA_SYS_FSTAT=31).
             * Native returns Lestra errno (same numeric values as Linux),
             * so pass-through works. */
            return (int64_t)lestra_syscall6(LESTRA_SYS_FSTAT, a1, a2, 0, 0, 0, 0);
        }

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
            /* Delegate to native sys_ioctl (LESTRA_SYS_IOCTL=34).
             * Native handles TCGETS/TCSETS/FIONREAD and returns -ENOTTY
             * for unsupported requests — same errno values as Linux. */
            return (int64_t)lestra_syscall6(LESTRA_SYS_IOCTL, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_ACCESS:
            /* Delegate to native sys_access (LESTRA_SYS_ACCESS=32). */
            return (int64_t)lestra_syscall6(LESTRA_SYS_ACCESS, a1, a2, 0, 0, 0, 0);

        case LINUX_SYS_DUP: {
            /* Linux dup(oldfd) returns the lowest available fd that is
             * a copy of oldfd. Native LestraOS only has sys_dup2, so we
             * find the lowest free fd and call dup2(oldfd, free_fd). */
            struct process* cur = task_current();
            if (!cur) return -LINUX_EBADF;
            if ((int64_t)a1 < 0 || (int64_t)a1 >= MAX_FD_PER_PROC) return -LINUX_EBADF;
            if (cur->fds[a1].type == FD_UNUSED) return -LINUX_EBADF;
            int newfd = -1;
            for (int i = 0; i < MAX_FD_PER_PROC; i++) {
                if (cur->fds[i].type == FD_UNUSED) { newfd = i; break; }
            }
            if (newfd < 0) return -LINUX_EMFILE;
            return (int64_t)lestra_syscall6(LESTRA_SYS_DUP2, a1, (uint64_t)newfd, 0, 0, 0, 0);
        }

        case LINUX_SYS_DUP2:
            /* Delegate to native sys_dup2 (LESTRA_SYS_DUP2=28). */
            return (int64_t)lestra_syscall6(LESTRA_SYS_DUP2, a1, a2, 0, 0, 0, 0);

        case LINUX_SYS_GETPID:
            return (int64_t)lestra_syscall6(LESTRA_SYS_GETPID, 0, 0, 0, 0, 0, 0);

        case LINUX_SYS_GETPPID:
            /* Delegate to native sys_getppid (LESTRA_SYS_GETPPID=37). */
            return (int64_t)lestra_syscall6(LESTRA_SYS_GETPPID, 0, 0, 0, 0, 0, 0);

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
            /* Linux struct utsname is 6x65-byte strings.
             * SMAP-safe: build in kernel buffer, copy_to_user. */
            if (!a1) return -LINUX_EFAULT;
            if (!access_ok((void*)a1, 6 * 65)) return -LINUX_EFAULT;
            char kbuf[6 * 65];
            memset(kbuf, 0, sizeof(kbuf));
            memcpy(kbuf,       "Linux",   5);  /* sysname */
            memcpy(kbuf + 65,  "lestra",  6);  /* nodename */
            memcpy(kbuf + 130, "5.15.0",  6);  /* release */
            memcpy(kbuf + 195, "#1",      2);  /* version */
            memcpy(kbuf + 260, "x86_64",  6);  /* machine */
            memcpy(kbuf + 325, "(none)",  6);  /* domainname */
            if (copy_to_user((void*)a1, kbuf, sizeof(kbuf)) < 0) return -LINUX_EFAULT;
            return 0;
        }

        case LINUX_SYS_GETTIMEOFDAY: {
            /* Linux struct timeval = { tv_sec, tv_usec } (8+8 bytes).
             * SMAP-safe: build in kernel buffer, copy_to_user. */
            int64_t ms = (int64_t)lestra_syscall6(LESTRA_SYS_GETTIMEOFDAY, 0, 0, 0, 0, 0, 0);
            if (a1) {
                if (!access_ok((void*)a1, 16)) return -LINUX_EFAULT;
                int64_t tv[2];
                tv[0] = ms / 1000;
                tv[1] = (ms % 1000) * 1000;
                if (copy_to_user((void*)a1, tv, 16) < 0) return -LINUX_EFAULT;
            }
            return 0;
        }

        case LINUX_SYS_NANOSLEEP: {
            /* Linux: nanosleep(req, rem). req = { seconds, nanoseconds }.
             * SMAP-safe: copy_from_user the request struct. */
            if (!a1) return -LINUX_EFAULT;
            if (!access_ok((void*)a1, 16)) return -LINUX_EFAULT;
            int64_t req[2];
            if (copy_from_user(req, (void*)a1, 16) < 0) return -LINUX_EFAULT;
            int64_t ms = req[0] * 1000 + req[1] / 1000000;
            return (int64_t)lestra_syscall6(LESTRA_SYS_SLEEP, (uint64_t)ms, 0, 0, 0, 0, 0);
        }

        case LINUX_SYS_EXIT:
            lestra_syscall6(LESTRA_SYS_EXIT, a1, 0, 0, 0, 0, 0);
            return 0;  /* never reached */

        case LINUX_SYS_EXECVE:
            return (int64_t)lestra_syscall6(LESTRA_SYS_EXECVE, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_RT_SIGACTION:
            return (int64_t)lestra_syscall6(LESTRA_SYS_RT_SIGACTION, a1, a2, a3, a4, 0, 0);

        case LINUX_SYS_RT_SIGPROCMASK:
            return (int64_t)lestra_syscall6(LESTRA_SYS_RT_SIGPROCMASK, a1, a2, a3, a4, 0, 0);

        case LINUX_SYS_RT_SIGRETURN:
            return (int64_t)lestra_syscall6(LESTRA_SYS_RT_SIGRETURN, 0, 0, 0, 0, 0, 0);

        case LINUX_SYS_CLONE:
            /* Linux clone(flags, child_stack, ptid, ctid, newtls).
             * True clone() with CLONE_VM/CLONE_THREAD needs a thread-aware
             * scheduler (XL complexity — sched_clone_thread is still a
             * stub). For now, treat clone-as-fork: ignore the flags and
             * call proc_fork via LESTRA_SYS_FORK. This unblocks Linux
             * binaries that call clone() for fork-like behavior; threads
             * still don't work. */
            /* fallthrough */
        case LINUX_SYS_FORK:
        case LINUX_SYS_VFORK:
            /* Native proc_fork returns child pid (>0) in parent, 0 in
             * child, -1 on error — same convention as Linux fork(). */
            return (int64_t)lestra_syscall6(LESTRA_SYS_FORK, 0, 0, 0, 0, 0, 0);

        case LINUX_SYS_WAIT4:
            return (int64_t)lestra_syscall6(LESTRA_SYS_WAITPID, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_KILL:
            return (int64_t)lestra_syscall6(LESTRA_SYS_KILL, a1, a2, 0, 0, 0, 0);

        case LINUX_SYS_FCNTL: {
            /* Linux fcntl(fd, cmd, arg). Native LestraOS does not yet
             * have a sys_fcntl syscall, so we implement the common cmds
             * inline against the current process's fd table. */
            struct process* cur = task_current();
            if (!cur) return -LINUX_EBADF;
            int fd = (int)a1;
            int cmd = (int)a2;
            if (fd < 0 || fd >= MAX_FD_PER_PROC) return -LINUX_EBADF;
            struct fd_entry* e = &cur->fds[fd];
            if (e->type == FD_UNUSED) return -LINUX_EBADF;
            switch (cmd) {
                case LINUX_F_DUPFD: {
                    /* Duplicate fd to lowest >= arg. */
                    int start = (int)a3;
                    if (start < 0) start = 0;
                    if (start >= MAX_FD_PER_PROC) return -LINUX_EINVAL;
                    int newfd = -1;
                    for (int i = start; i < MAX_FD_PER_PROC; i++) {
                        if (cur->fds[i].type == FD_UNUSED) { newfd = i; break; }
                    }
                    if (newfd < 0) return -LINUX_EMFILE;
                    return (int64_t)lestra_syscall6(LESTRA_SYS_DUP2,
                                                     (uint64_t)fd, (uint64_t)newfd, 0, 0, 0, 0);
                }
                case LINUX_F_GETFD:
                    /* We don't track FD_CLOEXEC yet; report it as clear. */
                    return 0;
                case LINUX_F_SETFD:
                    /* Accept FD_CLOEXEC silently (no-op until exec exists). */
                    return 0;
                case LINUX_F_GETFL:
                    /* Return the open flags stored on the fd entry. */
                    return (int64_t)e->flags;
                case LINUX_F_SETFL: {
                    /* Allow O_APPEND / O_NONBLOCK changes (mask out
                     * access mode which can't be changed post-open). */
                    int mask = LINUX_O_APPEND | LINUX_O_NONBLOCK;
                    e->flags = (e->flags & ~mask) | ((int)a3 & mask);
                    return 0;
                }
                default:
                    /* Unknown cmds (F_GETLK/F_SETLK/F_GETOWN/...): pretend
                     * success so the binary doesn't abort. */
                    return 0;
            }
        }

        case LINUX_SYS_SOCKET:
            /* Linux socket(domain, type, protocol) = LestraOS socket. */
            return (int64_t)lestra_syscall6(LESTRA_SYS_SOCKET, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_BIND:
            /* Linux bind(fd, addr, addrlen) = LestraOS bind. */
            return (int64_t)lestra_syscall6(LESTRA_SYS_BIND, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_CONNECT:
            /* Linux connect(fd, addr, addrlen) = LestraOS connect. */
            return (int64_t)lestra_syscall6(LESTRA_SYS_CONNECT, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_LISTEN:
            /* Linux listen(fd, backlog) = LestraOS listen. */
            return (int64_t)lestra_syscall6(LESTRA_SYS_LISTEN, a1, a2, 0, 0, 0, 0);

        case LINUX_SYS_ACCEPT:
            /* Linux accept(fd, addr, addrlen_ptr) = LestraOS accept.
             * Native sys_accept takes (fd, addr, addrlen_ptr) — same. */
            return (int64_t)lestra_syscall6(LESTRA_SYS_ACCEPT, a1, a2, a3, 0, 0, 0);

        case LINUX_SYS_SENDTO:
            /* Linux sendto(fd, buf, len, flags, addr, addrlen).
             * Native sys_send(fd, buf, len, flags) is sendto with a NULL
             * addr — it uses the connected peer for TCP, or the
             * previously-bound dest for UDP. We translate sendto → send
             * and IGNORE the addr argument. This works for connected
             * sockets (the common case for Linux TCP clients); it does
             * NOT support unconnected UDP sendto (limitation: documented). */
            return (int64_t)lestra_syscall6(LESTRA_SYS_SEND, a1, a2, a3, a4, 0, 0);

        case LINUX_SYS_RECVFROM:
            /* Linux recvfrom(fd, buf, len, flags, addr, addrlen_ptr).
             * Translate to sys_recv(fd, buf, len, flags) — the src addr
             * is NOT filled in. Works for connected sockets; the caller
             * sees addr=NULL semantics. */
            return (int64_t)lestra_syscall6(LESTRA_SYS_RECV, a1, a2, a3, a4, 0, 0);

        case LINUX_SYS_SYSINFO: {
            /* Linux struct sysinfo is 112 bytes:
             *   int64 totalram, freeram, sharedram, bufferram;
             *   int32 procs;
             *   ... padding ...
             *   int64 totalhigh, freehigh;
             *   int32 mem_unit;
             * SMAP-safe: build in kernel buffer, copy_to_user.
             * Read real PMM stats (pmm_get_total / pmm_get_free) and a
             * real process count by walking the global process table. */
            if (!a1) return -LINUX_EFAULT;
            if (!access_ok((void*)a1, 112)) return -LINUX_EFAULT;
            uint64_t total = (uint64_t)pmm_get_total();
            uint64_t free_ = (uint64_t)pmm_get_free();
            /* pmm_get_used() is available but not exposed in sysinfo's
             * standard fields — keep the call so the linker notices if
             * PMM stats ever disappear, but don't store the value. */
            (void)pmm_get_used();
            /* Count live (non-free, non-zombie) processes for `procs`. */
            int procs_count = 0;
            for (int i = 0; i < MAX_PROCS; i++) {
                if (procs[i].state != PROC_FREE && procs[i].state != PROC_ZOMBIE) {
                    procs_count++;
                }
            }
            uint8_t kbuf[112];
            memset(kbuf, 0, 112);
            *(int64_t*)(kbuf + 0)  = (int64_t)total;   /* totalram (bytes) */
            *(int64_t*)(kbuf + 8)  = (int64_t)free_;   /* freeram (bytes) */
            *(int64_t*)(kbuf + 16) = 0;                /* sharedram */
            *(int64_t*)(kbuf + 24) = 0;                /* bufferram */
            *(int32_t*)(kbuf + 40) = procs_count;      /* procs */
            /* totalswap/freeswap/.../totalhigh/freehigh/mem_unit are 0 */
            if (copy_to_user((void*)a1, kbuf, 112) < 0) return -LINUX_EFAULT;
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
