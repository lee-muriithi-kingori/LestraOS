/*
 * Lestra OS - System Call Interface
 * Copyright (c) 2026 lestramk.org
 */

#ifndef LESTRA_SYSCALL_H
#define LESTRA_SYSCALL_H

#include <lestra/types.h>

/* Syscall numbers */
#define SYS_EXIT        0
#define SYS_FORK        1
#define SYS_READ        2
#define SYS_WRITE       3
#define SYS_OPEN        4
#define SYS_CLOSE       5
#define SYS_WAITPID     6
#define SYS_EXECVE      7
#define SYS_GETPID      8
#define SYS_BRK         9
#define SYS_MMAP        10
#define SYS_MUNMAP      11
#define SYS_GETTIMEOFDAY 12
#define SYS_SLEEP       13
#define SYS_GETCWD      14
#define SYS_CHDIR       15
#define SYS_MKDIR       16
#define SYS_RMDIR       17
#define SYS_STAT        18
#define SYS_LSEEK       19
#define SYS_GETDENTS    20
#define SYS_REBOOT      21
#define SYS_UNAME       22
#define SYS_MAX         23

/* Syscall handler */
void syscall_init(void);
int64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5);

#endif /* LESTRA_SYSCALL_H */
