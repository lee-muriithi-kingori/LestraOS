/*
 * Lestra OS - unistd Implementation
 * Copyright (c) 2026 lestramk.org
 *
 * FIX: previous syscall() was variadic with NO input constraints in the
 * asm block, leaving GCC free to put num/a1..a5 in any register. That
 * means the on-entry register state was effectively random, so every
 * libc wrapper (getpid, read, write, ...) would call the wrong syscall
 * with garbage arguments. Replace with an explicit 6-arg prototype and
 * proper input bindings matching the syscall ABI:
 *   num -> rax, a1 -> rdi, a2 -> rsi, a3 -> rdx,
 *   a4 -> r10, a5 -> r8  (syscall clobbers rcx and r11)
 */

#include <unistd.h>

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

pid_t getpid(void) {
    return (pid_t)syscall(8, 0, 0, 0, 0, 0);   /* SYS_GETPID */
}

ssize_t write(int fd, const void* buf, size_t count) {
    return (ssize_t)syscall(3, (uint64_t)fd, (uint64_t)buf, (uint64_t)count, 0, 0);
}

ssize_t read(int fd, void* buf, size_t count) {
    return (ssize_t)syscall(2, (uint64_t)fd, (uint64_t)buf, (uint64_t)count, 0, 0);
}

int open(const char* pathname, int flags, ...) {
    return (int)syscall(4, (uint64_t)pathname, (uint64_t)flags, 0, 0, 0);
}

int close(int fd) {
    return (int)syscall(5, (uint64_t)fd, 0, 0, 0, 0);
}

unsigned int sleep(unsigned int seconds) {
    syscall(13, (uint64_t)seconds * 1000, 0, 0, 0, 0);  /* SYS_SLEEP in ms */
    return 0;
}

void _exit(int status) {
    syscall(0, (uint64_t)status, 0, 0, 0, 0);
    while (1);
}

/* --- Stubs for the rest of the POSIX surface declared in unistd.h.
 * The kernel syscalls for these exist but mostly return -1 (not yet
 * implemented). Provide libc wrappers so user ELFs link cleanly even
 * before the kernel side is fleshed out. */

pid_t fork(void) {
    return (pid_t)syscall(1, 0, 0, 0, 0, 0);              /* SYS_FORK */
}

pid_t getppid(void) {
    /* No dedicated syscall yet; return 1 (init) as a sane default. */
    return 1;
}

int execve(const char* pathname, char* const argv[], char* const envp[]) {
    return (int)syscall(7, (uint64_t)pathname,
                          (uint64_t)argv,
                          (uint64_t)envp, 0, 0);          /* SYS_EXECVE */
}

int execv(const char* pathname, char* const argv[]) {
    return execve(pathname, argv, (char* const*)0);
}

int execvp(const char* file, char* const argv[]) {
    /* PATH lookup not implemented; fall through to execv on the raw name. */
    return execv(file, argv);
}

pid_t waitpid(pid_t pid, int* status, int options) {
    return (pid_t)syscall(6, (uint64_t)(int32_t)pid,
                            (uint64_t)status,
                            (uint64_t)options, 0, 0);     /* SYS_WAITPID */
}

off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)syscall(19, (uint64_t)fd,
                            (uint64_t)offset,
                            (uint64_t)whence, 0, 0);     /* SYS_LSEEK */
}

int chdir(const char* path) {
    return (int)syscall(15, (uint64_t)path, 0, 0, 0, 0);  /* SYS_CHDIR */
}

char* getcwd(char* buf, size_t size) {
    int rc = (int)syscall(14, (uint64_t)buf, (uint64_t)size, 0, 0, 0);
    return (rc < 0) ? (char*)0 : buf;
}

int mkdir(const char* pathname, uint32_t mode) {
    return (int)syscall(16, (uint64_t)pathname, (uint64_t)mode, 0, 0, 0);
}

int rmdir(const char* pathname) {
    return (int)syscall(17, (uint64_t)pathname, 0, 0, 0, 0);
}

int usleep(unsigned int usec) {
    /* Kernel SYS_SLEEP takes ms. Round up to the nearest ms. */
    uint64_t ms = (usec + 999u) / 1000u;
    syscall(13, ms, 0, 0, 0, 0);
    return 0;
}

int access(const char* pathname, int mode) {
    /* No dedicated syscall; fake success if path is non-NULL. */
    (void)mode;
    return pathname ? 0 : -1;
}

int unlink(const char* pathname) {
    /* SYS_UNLINK (29) — now implemented in the kernel for memfs files. */
    return (int)syscall(29, (uint64_t)pathname, 0, 0, 0, 0);
}

int rename(const char* oldpath, const char* newpath) {
    (void)oldpath; (void)newpath;
    return -1;
}

long sysconf(int name) {
    /* _SC_PAGESIZE is the only one commonly used. */
    switch (name) {
        case 1:  return 4096;   /* _SC_PAGESIZE */
        case 2:  return 256;    /* _SC_OPEN_MAX (matches MAX_OPEN_FILES) */
        default: return -1;
    }
}

int pipe(int pipefd[2]) {
    return (int)syscall(23, (uint64_t)pipefd, 0, 0, 0, 0);  /* SYS_PIPE */
}

int dup2(int oldfd, int newfd) {
    return (int)syscall(28, (uint64_t)oldfd, (uint64_t)newfd, 0, 0, 0);  /* SYS_DUP2 */
}
