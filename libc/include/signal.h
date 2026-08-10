/*
 * Lestra OS - C Standard Library - signal (POSIX)
 * Copyright (c) 2026 lestramk.org
 *
 * W1-A finding F: previously the libc had no <signal.h>, so programs
 * that referenced SIGHUP/SIGINT/SIGTERM/SIGKILL had to hand-define
 * them.  The numbers below match the Linux x86_64 ABI (signums 1..31
 * are the classic Unix signals; 32+ are the Linux real-time signals).
 *
 * The kernel already implements SYS_KILL(24), SYS_RT_SIGACTION(25),
 * and SYS_RT_SIGPROCMASK(26) — wrappers live in libc/src/unistd.c.
 *
 * Freestanding — no FP, no SSE.
 */
#ifndef LIBC_SIGNAL_H
#define LIBC_SIGNAL_H

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>   /* for pid_t */

/* Signal numbers (Linux x86_64 — 1..31 are the classic signals). */
#define SIGHUP      1   /* Hangup (POSIX) */
#define SIGINT      2   /* Interrupt (ANSI) */
#define SIGQUIT     3   /* Quit (POSIX) */
#define SIGILL      4   /* Illegal instruction (ANSI) */
#define SIGTRAP     5   /* Trace trap (POSIX) */
#define SIGABRT     6   /* Abort (ANSI) */
#define SIGIOT      6   /* IOT trap (4.2 BSD, alias for SIGABRT) */
#define SIGBUS      7   /* BUS error (4.2 BSD) */
#define SIGFPE      8   /* Floating-point exception (ANSI) */
#define SIGKILL     9   /* Kill, unblockable (POSIX) */
#define SIGUSR1    10   /* User-defined signal 1 (POSIX) */
#define SIGSEGV    11   /* Segmentation violation (ANSI) */
#define SIGUSR2    12   /* User-defined signal 2 (POSIX) */
#define SIGPIPE    13   /* Broken pipe (POSIX) */
#define SIGALRM    14   /* Alarm clock (POSIX) */
#define SIGTERM    15   /* Termination (ANSI) */
#define SIGSTKFLT  16   /* Stack fault */
#define SIGCHLD    17   /* Child status has changed (POSIX) */
#define SIGCONT    18   /* Continue (POSIX) */
#define SIGSTOP    19   /* Stop, unblockable (POSIX) */
#define SIGTSTP    20   /* Keyboard stop (POSIX) */
#define SIGTTIN    21   /* Background read from tty (POSIX) */
#define SIGTTOU    22   /* Background write to tty (POSIX) */
#define SIGURG     23   /* Urgent condition on socket (4.2 BSD) */
#define SIGXCPU    24   /* CPU limit exceeded (4.2 BSD) */
#define SIGXFSZ    25   /* File size limit exceeded (4.2 BSD) */
#define SIGVTALRM  26   /* Virtual alarm clock (4.2 BSD) */
#define SIGPROF    27   /* Profiling alarm clock (4.2 BSD) */
#define SIGWINCH   28   /* Window size change (4.3 BSD, Sun) */
#define SIGIO      29   /* I/O now possible (4.2 BSD) */
#define SIGPOLL    29   /* Pollable event occurred (System V) — alias */
#define SIGPWR     30   /* Power failure restart (System V) */
#define SIGSYS     31   /* Bad system call (System V) */
#define SIGRTMIN   32
#define SIGRTMAX   64   /* kernel currently doesn't deliver rt signals */
#define SIGBAD     255  /* invalid signal sentinel */

/* Signal handler dispositions. */
#define SIG_DFL  ((sighandler_t)0)
#define SIG_IGN  ((sighandler_t)1)
#define SIG_ERR  ((sighandler_t)-1)

/* sigprocmask() how */
#define SIG_BLOCK     0
#define SIG_UNBLOCK   1
#define SIG_SETMASK   2

/* The kernel passes no info struct; the sa_handler field is used. */
typedef void (*sighandler_t)(int);

/* Minimal sigaction structure — matches what the kernel's
 * sys_rt_sigaction reads/writes via the user pointer. */
typedef struct sigaction {
    sighandler_t sa_handler;
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    /* sa_mask is a 64-bit bitmask of blocked signals during the
     * handler. The kernel's signal_sigaction() reads it as a
     * uint64_t at offset 16 (after handler + flags). */
    uint64_t sa_mask;
} sigaction_t;

/* Signal-set helpers (sigset_t is a 64-bit mask of 1..64). */
typedef uint64_t sigset_t;
#define SIGEMPTYSET(set)    (*(set) = 0)
#define SIGFILLSET(set)     (*(set) = (uint64_t)-1)
#define SIGADDSET(set, sig) (*(set) |= (uint64_t)1 << ((sig) - 1))
#define SIGDELSET(set, sig) (*(set) &= ~((uint64_t)1 << ((sig) - 1)))
#define SIGISMEMBER(set, sig) ((*(set) & ((uint64_t)1 << ((sig) - 1))) != 0)

/* Wrappers (defined in libc/src/unistd.c).  The kernel's
 * sys_rt_sigaction takes (signum, act, oldact, restorer); the libc
 * wrapper pulls the restorer out of the user's struct sigaction and
 * passes it through to the kernel.  The kernel's sys_rt_sigprocmask
 * takes (how, set, oldset, sigsetsize) — the libc wrapper passes 8
 * (sizeof(sigset_t)) for the size. */
sighandler_t signal(int signum, sighandler_t handler);
int kill(pid_t pid, int sig);
int sigaction(int signum, const sigaction_t* act, sigaction_t* oldact);
int sigprocmask(int how, const sigset_t* set, sigset_t* oldset);

#endif /* LIBC_SIGNAL_H */
