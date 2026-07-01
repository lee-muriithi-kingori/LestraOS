/*
 * Lestra OS - C Standard Library - unistd (POSIX)
 * Copyright (c) 2026 lestramk.org
 */
#ifndef LIBC_UNISTD_H
#define LIBC_UNISTD_H

#include <stddef.h>
#include <stdint.h>

typedef int64_t ssize_t;
typedef uint64_t size_t;
typedef int32_t pid_t;
typedef int64_t off_t;

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

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

/* Directory */
int chdir(const char* path);
char* getcwd(char* buf, size_t size);
int mkdir(const char* pathname, uint32_t mode);
int rmdir(const char* pathname);

/* Sleep */
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);

/* Misc */
int access(const char* pathname, int mode);
int unlink(const char* pathname);
int rename(const char* oldpath, const char* newpath);
long sysconf(int name);

/* Syscall wrapper */
int64_t syscall(uint64_t num, ...);

#endif /* LIBC_UNISTD_H */
