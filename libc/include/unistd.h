/*
 * Lestra OS - C Standard Library - unistd (POSIX)
 * Copyright (c) 2026 lestramk.org
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

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Syscall numbers (mirror kernel/include/lestra/syscall.h) */
#define SYS_EXIT         0
#define SYS_FORK         1
#define SYS_READ         2
#define SYS_WRITE        3
#define SYS_OPEN         4
#define SYS_CLOSE        5
#define SYS_WAITPID      6
#define SYS_EXECVE       7
#define SYS_GETPID       8
#define SYS_BRK          9
#define SYS_MMAP         10
#define SYS_MUNMAP       11
#define SYS_GETTIMEOFDAY 12
#define SYS_SLEEP        13
#define SYS_GETCWD       14
#define SYS_CHDIR        15
#define SYS_MKDIR        16
#define SYS_RMDIR        17
#define SYS_STAT         18
#define SYS_LSEEK        19
#define SYS_GETDENTS     20
#define SYS_REBOOT       21
#define SYS_UNAME        22
#define SYS_PIPE         23
#define SYS_KILL         24
#define SYS_RT_SIGACTION    25
#define SYS_RT_SIGPROCMASK  26
#define SYS_RT_SIGRETURN    27
#define SYS_DUP2            28
#define SYS_UNLINK           29
#define SYS_MAX             30

/* Process */
pid_t fork(void);
pid_t getpid(void);
pid_t getppid(void);
void _exit(int status);
int execve(const char* pathname, char* const argv[], char* const envp[]);
int execv(const char* pathname, char* const argv[]);
int execvp(const char* file, char* const argv[]);
pid_t waitpid(pid_t pid, int* status, int options);

/* File */
int open(const char* pathname, int flags, ...);
int close(int fd);
ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
off_t lseek(int fd, off_t offset, int whence);
int pipe(int pipefd[2]);
int dup2(int oldfd, int newfd);

/* Directory */
int chdir(const char* path);
char* getcwd(char* buf, size_t size);
int mkdir(const char* pathname, uint32_t mode);
int rmdir(const char* pathname);

/* Sleep */
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);

/* Seek constants (whence values for lseek) */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* Misc */
int access(const char* pathname, int mode);
int unlink(const char* pathname);
int rename(const char* oldpath, const char* newpath);
long sysconf(int name);

/* Syscall wrapper - explicit 6-arg form so the asm can bind every
 * argument to the correct syscall-ABI register (see libc/src/unistd.c). */
int64_t syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t a4, uint64_t a5);

#endif /* LIBC_UNISTD_H */
