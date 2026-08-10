/*
 * Lestra OS - unistd Implementation (POSIX)
 * Copyright (c) 2026 lestramk.org
 *
 * W3-A rewrite: every wrapper now follows POSIX return convention:
 *
 *   - On success: return the kernel's non-negative return value.
 *   - On error:   the kernel returns a negative errno (e.g. -ENOENT).
 *                 We translate that to `errno = -ret; return -1;`
 *                 (or NULL for pointer-typed wrappers like getcwd).
 *
 * Previously every wrapper returned the raw negative kernel errno,
 * which broke the standard `if (call() == -1) perror(...)` idiom: the
 * -1 test failed, the program proceeded with a bogus fd/pid, and
 * crashed.  With errno + the new wrappers, POSIX programs compile and
 * behave correctly.
 *
 * The `syscall()` ABI: num->rax, a1->rdi, a2->rsi, a3->rdx, a4->r10,
 * a5->r8 (rcx and r11 clobbered by syscall/sysret).  `syscall6()`
 * additionally passes a6 in r9, used by mmap(addr,len,prot,flags,
 * fd,offset).
 *
 * Freestanding — no FP, no SSE.  Builds with -mno-sse -mno-mmx -mno-sse2.
 */

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <stat.h>
#include <time.h>
#include <socket.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>

/* MAP_FAILED sentinel for mmap().  Lives here (not in a separate
 * <sys/mman.h>) to keep the libc header surface small. */
#ifndef MAP_FAILED
#define MAP_FAILED ((void*)-1)
#endif

/* ------------------------------------------------------------------ */
/* Syscall stubs                                                      */
/* ------------------------------------------------------------------ */

int64_t syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t a4, uint64_t a5) {
    int64_t result;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    __asm__ volatile (
        "syscall\n"
        : "=a"(result)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );
    return result;
}

int64_t syscall6(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                 uint64_t a4, uint64_t a5, uint64_t a6) {
    int64_t result;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    register uint64_t r9  __asm__("r9")  = a6;
    __asm__ volatile (
        "syscall\n"
        : "=a"(result)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return result;
}

/* ------------------------------------------------------------------ */
/* Process                                                            */
/* ------------------------------------------------------------------ */

pid_t fork(void) {
    int64_t r = syscall(SYS_FORK, 0, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (pid_t)r;
}

pid_t getpid(void) {
    int64_t r = syscall(SYS_GETPID, 0, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (pid_t)r;
}

/* W1-A finding E: previously returned 1 hardcoded.  The kernel
 * implements SYS_GETPPID(37) — call it. */
pid_t getppid(void) {
    int64_t r = syscall(SYS_GETPPID, 0, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (pid_t)r;
}

pid_t waitpid(pid_t pid, int* status, int options) {
    int64_t r = syscall(SYS_WAITPID, (uint64_t)(int64_t)pid,
                          (uint64_t)status, (uint64_t)options, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (pid_t)r;
}

uid_t getuid(void) {
    int64_t r = syscall(SYS_GETUID, 0, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return (uid_t)-1; }
    return (uid_t)r;
}

gid_t getgid(void) {
    int64_t r = syscall(SYS_GETGID, 0, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return (gid_t)-1; }
    return (gid_t)r;
}

int setuid(uid_t uid) {
    int64_t r = syscall(SYS_SETUID, (uint64_t)uid, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

void _exit(int status) {
    syscall(SYS_EXIT, (uint64_t)(int64_t)status, 0, 0, 0, 0);
    while (1) { __asm__ volatile("hlt"); }
}

int execve(const char* pathname, char* const argv[], char* const envp[]) {
    int64_t r = syscall(SYS_EXECVE, (uint64_t)pathname,
                          (uint64_t)argv, (uint64_t)envp, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    /* On success execve does not return; if it did, treat as success. */
    return 0;
}

int execv(const char* pathname, char* const argv[]) {
    return execve(pathname, argv, (char* const*)0);
}

int execvp(const char* file, char* const argv[]) {
    /* W1-A finding E: previously fell straight through to execv on the
     * raw name, so `execvp("ls", ...)` tried to exec "/ls" and failed.
     * Walk a minimal default PATH if `file` contains no '/' separator.
     * LestraOS libc has no `environ`, so we use a hardcoded
     * "/bin:/usr/bin:." default; the kernel's execve() resolves
     * relative paths against the process cwd. */
    if (!file || !*file) { errno = ENOENT; return -1; }

    /* If file contains a '/', exec it directly. */
    const char* slash = file;
    while (*slash && *slash != '/') slash++;
    if (*slash == '/') return execv(file, argv);

    /* PATH walk.  Buffer must hold "/<dir>/<file>\0".  We pick a
     * conservative 256-byte limit (matches MAX_PATH_LEN in the
     * kernel for the relative-path case). */
    static const char default_path[] = "/bin:/usr/bin:.";
    const char* path = default_path;
    while (*path) {
        char buf[256];
        unsigned i = 0;
        while (*path && *path != ':' && i + 1 < sizeof(buf)) {
            buf[i++] = *path++;
        }
        if (*path == ':') path++;
        if (i == 0) continue;     /* empty PATH element → skip */
        if (i + 1 + (unsigned)strlen(file) + 1 > sizeof(buf)) continue;
        buf[i++] = '/';
        unsigned j = 0;
        while (file[j] && i + 1 < sizeof(buf)) buf[i++] = file[j++];
        buf[i] = '\0';

        execv(buf, argv);
        /* If we got here, exec failed.  Keep going unless ENOENT —
         * i.e. only retry on "not found"; for EACCES we return
         * immediately so the user sees the real cause. */
        if (errno != ENOENT) return -1;
    }
    errno = ENOENT;
    return -1;
}

/* ------------------------------------------------------------------ */
/* File descriptors                                                   */
/* ------------------------------------------------------------------ */

int open(const char* pathname, int flags, ...) {
    /* The kernel's sys_open takes (path, flags) only — no mode arg.
     * Variadic signature is kept for source compatibility with
     * programs that pass O_CREAT's third mode argument. */
    int64_t r = syscall(SYS_OPEN, (uint64_t)pathname,
                          (uint64_t)flags, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

int close(int fd) {
    int64_t r = syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

ssize_t read(int fd, void* buf, size_t count) {
    int64_t r = syscall(SYS_READ, (uint64_t)fd, (uint64_t)buf,
                          (uint64_t)count, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (ssize_t)r;
}

ssize_t write(int fd, const void* buf, size_t count) {
    int64_t r = syscall(SYS_WRITE, (uint64_t)fd, (uint64_t)buf,
                          (uint64_t)count, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (ssize_t)r;
}

off_t lseek(int fd, off_t offset, int whence) {
    int64_t r = syscall(SYS_LSEEK, (uint64_t)fd, (uint64_t)offset,
                          (uint64_t)whence, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (off_t)r;
}

int pipe(int pipefd[2]) {
    int64_t r = syscall(SYS_PIPE, (uint64_t)pipefd, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int dup2(int oldfd, int newfd) {
    int64_t r = syscall(SYS_DUP2, (uint64_t)oldfd, (uint64_t)newfd, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

/* dup(): the kernel now implements SYS_DUP(53) (W3-B).  Returns the
 * new fd or -1/errno. */
int dup(int oldfd) {
    int64_t r = syscall(SYS_DUP, (uint64_t)oldfd, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

int ioctl(int fd, uint64_t request, ...) {
    /* The kernel's sys_ioctl takes (fd, request, arg).  We accept a
     * variadic third argument for source compatibility. */
    int64_t r = syscall(SYS_IOCTL, (uint64_t)fd, (uint64_t)request, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* fcntl(): the kernel now implements SYS_FCNTL(54) (W3-B).  The
 * prototype in <fcntl.h> is variadic; the third argument is required
 * for "setter" commands (F_DUPFD, F_SETFD, F_SETFL, F_SETOWN, F_SETLK,
 * F_SETLKW, F_SETSIG, F_NOTIFY, F_DUPFD_CLOEXEC) and ignored by
 * "getter" commands (F_GETFD, F_GETFL, F_GETOWN, F_GETLK, ...).
 *
 * We only invoke va_arg for setter commands so that callers using
 * the 2-arg form `fcntl(fd, F_GETFD)` don't read garbage off the
 * stack.  Per the System V AMD64 ABI, integer variadic args are
 * zero-extended to 64 bits by the caller, so reading as `long` is
 * safe for both `int` and pointer args. */
int fcntl(int fd, int cmd, ...) {
    long arg = 0;
    if (cmd == F_DUPFD || cmd == F_SETFD || cmd == F_SETFL ||
        cmd == F_SETOWN || cmd == F_SETLK || cmd == F_SETLKW ||
        cmd == F_SETSIG || cmd == F_NOTIFY || cmd == F_DUPFD_CLOEXEC ||
        cmd == F_SETLEASE || cmd == F_GETLK || cmd == F_GETOWN_EX ||
        cmd == F_SETOWN_EX || cmd == F_OFD_SETLK || cmd == F_OFD_SETLKW ||
        cmd == F_OFD_GETLK) {
        va_list ap;
        va_start(ap, cmd);
        arg = (long)va_arg(ap, long);
        va_end(ap);
    }
    int64_t r = syscall(SYS_FCNTL, (uint64_t)fd, (uint64_t)cmd,
                          (uint64_t)arg, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

int fchdir(int fd) {
    int64_t r = syscall(SYS_FCHDIR, (uint64_t)fd, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

mode_t umask(mode_t mask) {
    int64_t r = syscall(SYS_UMASK, (uint64_t)mask, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return (mode_t)-1; }
    return (mode_t)r;
}

/* ------------------------------------------------------------------ */
/* Filesystem — path operations                                       */
/* ------------------------------------------------------------------ */

int unlink(const char* pathname) {
    int64_t r = syscall(SYS_UNLINK, (uint64_t)pathname, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* W1-A finding E: previously returned -1 without ever calling the
 * kernel.  Now calls SYS_RENAME(33) and translates errno. */
int rename(const char* oldpath, const char* newpath) {
    int64_t r = syscall(SYS_RENAME, (uint64_t)oldpath,
                          (uint64_t)newpath, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* W1-A finding E: previously faked success (return 0) without calling
 * the kernel.  Now calls SYS_ACCESS(32). */
int access(const char* pathname, int mode) {
    int64_t r = syscall(SYS_ACCESS, (uint64_t)pathname,
                          (uint64_t)mode, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int chmod(const char* pathname, mode_t mode) {
    int64_t r = syscall(SYS_CHMOD, (uint64_t)pathname, (uint64_t)mode,
                          0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int chown(const char* pathname, uid_t uid, gid_t gid) {
    int64_t r = syscall(SYS_CHOWN, (uint64_t)pathname,
                          (uint64_t)uid, (uint64_t)gid, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int mkdir(const char* pathname, mode_t mode) {
    int64_t r = syscall(SYS_MKDIR, (uint64_t)pathname, (uint64_t)mode,
                          0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int rmdir(const char* pathname) {
    int64_t r = syscall(SYS_RMDIR, (uint64_t)pathname, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int chdir(const char* path) {
    int64_t r = syscall(SYS_CHDIR, (uint64_t)path, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int chroot(const char* path) {
    int64_t r = syscall(SYS_CHROOT, (uint64_t)path, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

char* getcwd(char* buf, size_t size) {
    int64_t r = syscall(SYS_GETCWD, (uint64_t)buf, (uint64_t)size, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return NULL; }
    return buf;
}

int truncate(const char* path, off_t length) {
    int64_t r = syscall(SYS_TRUNCATE, (uint64_t)path, (uint64_t)length,
                          0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int ftruncate(int fd, off_t length) {
    int64_t r = syscall(SYS_FTRUNCATE, (uint64_t)fd, (uint64_t)length,
                          0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int symlink(const char* target, const char* linkpath) {
    int64_t r = syscall(SYS_SYMLINK, (uint64_t)target, (uint64_t)linkpath,
                          0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int link(const char* oldpath, const char* newpath) {
    int64_t r = syscall(SYS_LINK, (uint64_t)oldpath, (uint64_t)newpath,
                          0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

ssize_t readlink(const char* path, char* buf, size_t bufsiz) {
    int64_t r = syscall(SYS_READLINK, (uint64_t)path, (uint64_t)buf,
                          (uint64_t)bufsiz, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (ssize_t)r;
}

/* ------------------------------------------------------------------ */
/* stat / directory listing                                           */
/* ------------------------------------------------------------------ */

int stat(const char* pathname, struct stat* st) {
    int64_t r = syscall(SYS_STAT, (uint64_t)pathname, (uint64_t)st, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int fstat(int fd, struct stat* st) {
    int64_t r = syscall(SYS_FSTAT, (uint64_t)fd, (uint64_t)st, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* lstat: the kernel has no dedicated SYS_LSTAT (W1-A finding H).
 * The kernel's VFS doesn't follow symlinks yet (no symlink support),
 * so lstat() behaves identically to stat() for now.  When the kernel
 * gains symlink support, swap this for a real SYS_LSTAT call. */
int lstat(const char* pathname, struct stat* st) {
    return stat(pathname, st);
}

ssize_t getdents(int fd, void* dirp, size_t count) {
    int64_t r = syscall(SYS_GETDENTS, (uint64_t)fd, (uint64_t)dirp,
                          (uint64_t)count, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (ssize_t)r;
}

/* ------------------------------------------------------------------ */
/* Signals                                                            */
/* ------------------------------------------------------------------ */

int kill(pid_t pid, int sig) {
    int64_t r = syscall(SYS_KILL, (uint64_t)(int64_t)pid,
                          (uint64_t)sig, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* sigaction(): the kernel's sys_rt_sigaction takes a 4th `restorer`
 * argument that the user supplies inside struct sigaction.sa_restorer.
 * POSIX's libc sigaction() takes only 3 args — we extract sa_restorer
 * from `act` and pass it to the kernel. */
int sigaction(int signum, const sigaction_t* act, sigaction_t* oldact) {
    uint64_t restorer = 0;
    if (act && act->sa_restorer) {
        restorer = (uint64_t)act->sa_restorer;
    }
    int64_t r = syscall(SYS_RT_SIGACTION, (uint64_t)signum,
                          (uint64_t)act, (uint64_t)oldact,
                          restorer, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* sigprocmask(): the kernel's sys_rt_sigprocmask takes a 4th
 * `sigsetsize` argument.  We pass sizeof(sigset_t) = 8. */
int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    int64_t r = syscall(SYS_RT_SIGPROCMASK, (uint64_t)how,
                          (uint64_t)set, (uint64_t)oldset,
                          (uint64_t)sizeof(sigset_t), 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* signal(): the BSD/POSIX convenience wrapper around sigaction().
 * Installs `handler` as sa_handler with an empty mask and default
 * flags, returns the previous handler (or SIG_ERR on error). */
sighandler_t signal(int signum, sighandler_t handler) {
    sigaction_t act;
    sigaction_t old;
    /* Zero the struct first. */
    memset(&act, 0, sizeof(act));
    act.sa_handler  = handler;
    act.sa_flags    = 0;
    act.sa_restorer = NULL;
    act.sa_mask     = 0;
    if (sigaction(signum, &act, &old) < 0) return SIG_ERR;
    return old.sa_handler;
}

/* ------------------------------------------------------------------ */
/* Memory                                                             */
/* ------------------------------------------------------------------ */

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    int64_t r = syscall6(SYS_MMAP, (uint64_t)addr, (uint64_t)length,
                          (uint64_t)prot, (uint64_t)flags,
                          (uint64_t)fd, (uint64_t)offset);
    if (r < 0) { errno = (int)-r; return MAP_FAILED; }
    return (void*)r;
}

int munmap(void* addr, size_t length) {
    int64_t r = syscall(SYS_MUNMAP, (uint64_t)addr, (uint64_t)length,
                          0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* brk(addr): set the program break to addr.  Returns 0 on success and
 * -1/ENOMEM on failure.  The kernel's sys_brk returns the new break
 * (which equals addr on success, or the current break on failure) —
 * we translate that to the libc convention. */
int brk(void* addr) {
    int64_t r = syscall(SYS_BRK, (uint64_t)addr, 0, 0, 0, 0);
    if (r == 0) { errno = ENOMEM; return -1; }
    if ((uint64_t)r < (uint64_t)addr) { errno = ENOMEM; return -1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Clock / time                                                       */
/* ------------------------------------------------------------------ */

int gettimeofday(struct timeval* tv, struct timezone* tz) {
    int64_t r = syscall(SYS_GETTIMEOFDAY, (uint64_t)tv, (uint64_t)tz,
                          0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int clock_gettime(int clk_id, struct timespec* tp) {
    int64_t r = syscall(SYS_CLOCK_GETTIME, (uint64_t)clk_id,
                          (uint64_t)tp, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* nanosleep: the kernel has no dedicated SYS_NANOSLEEP (W1-A finding
 * H), only SYS_SLEEP (ms resolution).  We convert the requested
 * timespec to ms (rounding up) and call sleep().  rem, if non-NULL,
 * is zeroed on success — we can't query the unslept remainder. */
int nanosleep(const struct timespec* req, struct timespec* rem) {
    if (!req) { errno = EFAULT; return -1; }
    uint64_t ms = (uint64_t)req->tv_sec * 1000ull;
    /* tv_nsec is [0, 999999999]; convert to ms, rounding up so a 1ns
     * sleep still yields the CPU. */
    ms += ((uint64_t)req->tv_nsec + 999999ull) / 1000000ull;
    int64_t r = syscall(SYS_SLEEP, ms, 0, 0, 0, 0);
    if (r < 0) {
        errno = (int)-r;
        if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
        return -1;
    }
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    /* Kernel SYS_SLEEP takes ms.  POSIX returns the number of unslept
     * seconds on EINTR; we have no way to query that from the kernel,
     * so we return 0 on normal completion.  If the kernel returns
     * -EINTR we surface it via errno but still return 0. */
    int64_t r = syscall(SYS_SLEEP, (uint64_t)seconds * 1000ull, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return 0; }
    return 0;
}

int usleep(unsigned int usec) {
    /* Kernel SYS_SLEEP takes ms; round up so a 1us sleep still yields. */
    uint64_t ms = ((uint64_t)usec + 999u) / 1000u;
    int64_t r = syscall(SYS_SLEEP, ms, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* System info                                                        */
/* ------------------------------------------------------------------ */

int uname(void* buf) {
    int64_t r = syscall(SYS_UNAME, (uint64_t)buf, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int reboot(int cmd) {
    int64_t r = syscall(SYS_REBOOT, (uint64_t)cmd, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Sockets                                                            */
/* ------------------------------------------------------------------ */

int socket(int domain, int type, int protocol) {
    int64_t r = syscall(SYS_SOCKET, (uint64_t)domain, (uint64_t)type,
                          (uint64_t)protocol, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

int bind(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    int64_t r = syscall(SYS_BIND, (uint64_t)fd, (uint64_t)addr,
                          (uint64_t)addrlen, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int connect(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    int64_t r = syscall(SYS_CONNECT, (uint64_t)fd, (uint64_t)addr,
                          (uint64_t)addrlen, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int listen(int fd, int backlog) {
    int64_t r = syscall(SYS_LISTEN, (uint64_t)fd, (uint64_t)backlog,
                          0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int accept(int fd, struct sockaddr* addr, socklen_t* addrlen) {
    int64_t r = syscall(SYS_ACCEPT, (uint64_t)fd, (uint64_t)addr,
                          (uint64_t)addrlen, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

ssize_t send(int fd, const void* buf, size_t len, int flags) {
    int64_t r = syscall(SYS_SEND, (uint64_t)fd, (uint64_t)buf,
                          (uint64_t)len, (uint64_t)flags, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (ssize_t)r;
}

ssize_t recv(int fd, void* buf, size_t len, int flags) {
    int64_t r = syscall(SYS_RECV, (uint64_t)fd, (uint64_t)buf,
                          (uint64_t)len, (uint64_t)flags, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (ssize_t)r;
}

/* sendto / recvfrom: the kernel has no dedicated syscalls for these
 * (W1-A finding H — only sys_send/sys_recv).  Fall back to send/recv
 * when the destination/source address is NULL; otherwise return
 * -ENOSYS so callers know the operation isn't supported. */
ssize_t sendto(int fd, const void* buf, size_t len, int flags,
               const struct sockaddr* dest_addr, socklen_t dest_len) {
    if (dest_addr) {
        (void)dest_len;
        errno = ENOSYS;
        return -1;
    }
    return send(fd, buf, len, flags);
}

ssize_t recvfrom(int fd, void* buf, size_t len, int flags,
                 struct sockaddr* src_addr, socklen_t* src_len) {
    if (src_addr) {
        (void)src_len;
        errno = ENOSYS;
        return -1;
    }
    return recv(fd, buf, len, flags);
}

/* ------------------------------------------------------------------ */
/* Multiplexed I/O                                                    */
/* ------------------------------------------------------------------ */

int poll(void* fds, uint64_t nfds, int timeout_ms) {
    int64_t r = syscall(SYS_POLL, (uint64_t)fds, (uint64_t)nfds,
                          (uint64_t)(int64_t)timeout_ms, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

int select(int nfds, void* readfds, void* writefds, void* exceptfds,
           const void* timeout) {
    int64_t r = syscall(SYS_SELECT, (uint64_t)nfds, (uint64_t)readfds,
                          (uint64_t)writefds, (uint64_t)exceptfds,
                          (uint64_t)timeout);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

/* ------------------------------------------------------------------ */
/* Resource limits + times                                            */
/* ------------------------------------------------------------------ */

int getrlimit(int resource, void* rlim) {
    int64_t r = syscall(SYS_GETRLIMIT, (uint64_t)resource,
                          (uint64_t)rlim, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int setrlimit(int resource, const void* rlim) {
    int64_t r = syscall(SYS_SETRLIMIT, (uint64_t)resource,
                          (uint64_t)rlim, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int times(void* buf) {
    int64_t r = syscall(SYS_TIMES, (uint64_t)buf, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

/* ------------------------------------------------------------------ */
/* Scheduling priorities                                              */
/* ------------------------------------------------------------------ */

int setpriority(int which, int who, int prio) {
    int64_t r = syscall(SYS_SETPRIORITY, (uint64_t)which, (uint64_t)who,
                          (uint64_t)prio, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

/* getpriority(): POSIX returns the priority on success and -1 on
 * error — but -1 is also a valid priority.  Callers must inspect
 * errno to disambiguate.  We mirror the glibc convention: clear
 * errno on entry, set it on error. */
int getpriority(int which, int who) {
    errno = 0;
    int64_t r = syscall(SYS_GETPRIORITY, (uint64_t)which, (uint64_t)who,
                          0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

int nice(int inc) {
    int64_t r = syscall(SYS_NICE, (uint64_t)inc, 0, 0, 0, 0);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

/* ------------------------------------------------------------------ */
/* sysconf                                                            */
/* ------------------------------------------------------------------ */

long sysconf(int name) {
    switch (name) {
        case _SC_PAGESIZE:   return 4096;
        case _SC_OPEN_MAX:   return 256;     /* matches MAX_OPEN_FILES */
        case _SC_CLK_TCK:    return 100;
        case _SC_ARG_MAX:    return 4096;
        case _SC_CHILD_MAX:  return 64;
        case _SC_STREAM_MAX: return 16;
        case _SC_LINE_MAX:   return 4096;
        case _SC_VERSION:    return 200809L; /* _POSIX_VERSION */
        case _SC_JOB_CONTROL:  return 1;
        case _SC_SAVED_IDS:    return 1;
        default:            errno = EINVAL; return -1;
    }
}
