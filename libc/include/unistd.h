/*
 * Lestra OS - C Standard Library - unistd (POSIX)
 * Copyright (c) 2026 lestramk.org
 *
 * The SYS_* numbers below are mirrored 1:1 from
 * /home/z/lestraOS/kernel/include/lestra/syscall.h.  Keep them in
 * sync with that file whenever the kernel adds or renumbers a call.
 *
 * Wrapper convention (libc/src/unistd.c):
 *   - On success: return the kernel's positive return value.
 *   - On error:   set `errno = -ret` (kernel returns negative errno)
 *                 and return -1 (or NULL / 0 for pointer-typed calls).
 */
#ifndef LIBC_UNISTD_H
#define LIBC_UNISTD_H

#include <stddef.h>
#include <stdint.h>

#ifndef LIBC_SSIZE_T_DEFINED
#define LIBC_SSIZE_T_DEFINED
typedef int64_t ssize_t;
#endif
#ifndef LIBC_OFF_T_DEFINED
#define LIBC_OFF_T_DEFINED
typedef int64_t off_t;
#endif
typedef int32_t pid_t;
typedef int32_t uid_t;
typedef int32_t gid_t;
typedef uint32_t mode_t;

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Syscall numbers (mirror kernel/include/lestra/syscall.h — SYS_MAX=67).
 * W1-A finding F: previously libc only knew 0..29 with SYS_MAX=30,
 * dropping every syscall 30..52 (chmod/fstat/access/rename/ioctl/
 * getuid/getgid/getppid/setuid/times/clock_gettime/getrlimit/setrlimit/
 * futex/socket/bind/connect/listen/accept/send/recv/poll/select).
 * W3-B added 53..66 (dup/fcntl/truncate/ftruncate/chown/symlink/link/
 * readlink/chroot/fchdir/umask/setpriority/getpriority/nice). */
#define SYS_EXIT             0
#define SYS_FORK             1
#define SYS_READ             2
#define SYS_WRITE            3
#define SYS_OPEN             4
#define SYS_CLOSE            5
#define SYS_WAITPID          6
#define SYS_EXECVE           7
#define SYS_GETPID           8
#define SYS_BRK              9
#define SYS_MMAP            10
#define SYS_MUNMAP          11
#define SYS_GETTIMEOFDAY    12
#define SYS_SLEEP           13
#define SYS_GETCWD          14
#define SYS_CHDIR           15
#define SYS_MKDIR           16
#define SYS_RMDIR           17
#define SYS_STAT            18
#define SYS_LSEEK           19
#define SYS_GETDENTS        20
#define SYS_REBOOT          21
#define SYS_UNAME           22
#define SYS_PIPE            23
#define SYS_KILL            24
#define SYS_RT_SIGACTION    25
#define SYS_RT_SIGPROCMASK  26
#define SYS_RT_SIGRETURN    27
#define SYS_DUP2            28
#define SYS_UNLINK          29
#define SYS_CHMOD           30
#define SYS_FSTAT           31
#define SYS_ACCESS          32
#define SYS_RENAME          33
#define SYS_IOCTL           34
#define SYS_GETUID          35
#define SYS_GETGID          36
#define SYS_GETPPID         37
#define SYS_SETUID          38
#define SYS_TIMES           39
#define SYS_CLOCK_GETTIME   40
#define SYS_GETRLIMIT       41
#define SYS_SETRLIMIT       42
#define SYS_FUTEX           43
#define SYS_SOCKET          44
#define SYS_BIND            45
#define SYS_CONNECT         46
#define SYS_LISTEN          47
#define SYS_ACCEPT          48
#define SYS_SEND            49
#define SYS_RECV            50
#define SYS_POLL            51
#define SYS_SELECT          52
/* W3-B: POSIX gap fillers — the kernel now implements these too.
 * Keep in sync with kernel/include/lestra/syscall.h. */
#define SYS_DUP              53
#define SYS_FCNTL            54
#define SYS_TRUNCATE         55
#define SYS_FTRUNCATE        56
#define SYS_CHOWN            57
#define SYS_SYMLINK          58
#define SYS_LINK             59
#define SYS_READLINK         60
#define SYS_CHROOT           61
#define SYS_FCHDIR           62
#define SYS_UMASK            63
#define SYS_SETPRIORITY      64
#define SYS_GETPRIORITY      65
#define SYS_NICE             66
#define SYS_MAX              67

/* Process */
pid_t fork(void);
pid_t getpid(void);
pid_t getppid(void);
pid_t waitpid(pid_t pid, int* status, int options);
uid_t getuid(void);
gid_t getgid(void);
int   setuid(uid_t uid);
void  _exit(int status);
int   execve(const char* pathname, char* const argv[], char* const envp[]);
int   execv(const char* pathname, char* const argv[]);
int   execvp(const char* file, char* const argv[]);

/* File descriptors */
int     open(const char* pathname, int flags, ...);
int     close(int fd);
ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
off_t   lseek(int fd, off_t offset, int whence);
int     pipe(int pipefd[2]);
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);
int     ioctl(int fd, uint64_t request, ...);
int     fchdir(int fd);
mode_t  umask(mode_t mask);

/* Filesystem — path operations */
int     unlink(const char* pathname);
int     rename(const char* oldpath, const char* newpath);
int     access(const char* pathname, int mode);
int     chmod(const char* pathname, mode_t mode);
int     chown(const char* pathname, uid_t uid, gid_t gid);
int     mkdir(const char* pathname, mode_t mode);
int     rmdir(const char* pathname);
int     chdir(const char* path);
int     chroot(const char* path);
char*   getcwd(char* buf, size_t size);
int     truncate(const char* path, off_t length);
int     ftruncate(int fd, off_t length);
int     symlink(const char* target, const char* linkpath);
int     link(const char* oldpath, const char* newpath);
ssize_t readlink(const char* path, char* buf, size_t bufsiz);

/* stat / directory listing.
 * stat()/fstat()/lstat() prototypes live in <stat.h> (they take a
 * `struct stat*`, which would force <stat.h> on every translation
 * unit if declared here).  getdents() takes a raw buffer, so it
 * stays here. */
ssize_t getdents(int fd, void* dirp, size_t count);

/* Signals — kill() lives here per POSIX; sigaction()/sigprocmask()/
 * signal() live in <signal.h> with proper sigaction_t/sigset_t types. */
int     kill(pid_t pid, int sig);

/* Memory */
void*   mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int     munmap(void* addr, size_t length);
int     brk(void* addr);

/* Clock / time.  gettimeofday()/clock_gettime()/nanosleep() prototypes
 * live in <time.h> (they take `struct timeval*`/`struct timespec*`).
 * sleep()/usleep() take only scalars and stay here. */
unsigned int sleep(unsigned int seconds);
int     usleep(unsigned int usec);

/* System info — uname() takes a `struct utsname*` but we accept a
 * raw void* so unistd.h doesn't have to drag in <sys/utsname.h>.
 * Callers that want the typed struct can include the kernel's
 * utsname header; callers that just want the buffer filled (the
 * common case) can call it directly. */
int     uname(void* buf);

/* Sockets — all socket prototypes live in <socket.h>. */

/* Multiplexed I/O.  The kernel takes raw pointers for fd_set /
 * struct timespec; <time.h> defines struct timespec and the user
 * passes &ts.  fd_set is treated as a raw bit array of length
 * (nfds + 7) / 8 bytes. */
int     poll(void* fds, uint64_t nfds, int timeout_ms);
int     select(int nfds, void* readfds, void* writefds, void* exceptfds,
              const void* timeout);

/* Resource limits (struct rlimit — see <sys/resource.h> when added). */
int     getrlimit(int resource, void* rlim);
int     setrlimit(int resource, const void* rlim);

/* Process times — the typed `struct tms*` prototype lives in
 * <time.h>; here we keep the void* form so unistd.h stays
 * self-contained. */
int     times(void* buf);

/* Reboot */
int     reboot(int cmd);

/* Scheduling priorities (POSIX).  `which` is one of PRIO_PROCESS,
 * PRIO_PGRP, PRIO_USER (defined below). */
#define PRIO_PROCESS  0
#define PRIO_PGRP     1
#define PRIO_USER     2
int     setpriority(int which, int who, int prio);
int     getpriority(int which, int who);
int     nice(int inc);

/* Misc */
long    sysconf(int name);

/* Syscall stubs.
 *
 * `syscall()` is the explicit 6-arg form (num + a1..a5) that callers
 * already use; it binds num->rax, a1->rdi, a2->rsi, a3->rdx, a4->r10,
 * a5->r8 per the syscall ABI (rcx and r11 are clobbered).
 *
 * `syscall6()` is the 7-arg form (num + a1..a6) used by syscalls that
 * take a 6th argument, e.g. mmap(addr, len, prot, flags, fd, offset)
 * — a6 is passed in r9 per the syscall ABI. */
int64_t syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t a4, uint64_t a5);
int64_t syscall6(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                 uint64_t a4, uint64_t a5, uint64_t a6);

/* Seek constants (whence values for lseek) */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* access() mode bits */
#define F_OK  0   /* test for existence */
#define X_OK  1   /* test for execute */
#define W_OK  2   /* test for write */
#define R_OK  4   /* test for read */

/* sysconf() names */
#define _SC_PAGESIZE     1
#define _SC_OPEN_MAX     2
#define _SC_CLK_TCK      3
#define _SC_ARG_MAX      4
#define _SC_CHILD_MAX    5
#define _SC_NGROUPS_MAX  6
#define _SC_STREAM_MAX   7
#define _SC_TZNAME_MAX   8
#define _SC_JOB_CONTROL  9
#define _SC_SAVED_IDS   10
#define _SC_VERSION     11
#define _SC_LINE_MAX    12

#endif /* LIBC_UNISTD_H */
