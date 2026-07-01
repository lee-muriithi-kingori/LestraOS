/*
 * Lestra OS - unistd Implementation
 * Copyright (c) 2026 lestramk.org
 */

#include <unistd.h>

int64_t syscall(uint64_t num, ...) {
    int64_t result;
    __asm__ volatile (
        "mov %%rdi, %%rax\n"
        "mov %%rsi, %%rdi\n"
        "mov %%rdx, %%rsi\n"
        "mov %%rcx, %%rdx\n"
        "mov %%r8, %%r10\n"
        "mov %%r9, %%r8\n"
        "syscall\n"
        : "=a"(result)
        :
        : "rcx", "r11", "memory"
    );
    return result;
}

pid_t getpid(void) {
    return syscall(8);  /* SYS_GETPID */
}

ssize_t write(int fd, const void* buf, size_t count) {
    return syscall(3, fd, buf, count);  /* SYS_WRITE */
}

ssize_t read(int fd, void* buf, size_t count) {
    return syscall(2, fd, buf, count);  /* SYS_READ */
}

int open(const char* pathname, int flags) {
    return syscall(4, pathname, flags);  /* SYS_OPEN */
}

int close(int fd) {
    return syscall(5, fd);  /* SYS_CLOSE */
}

unsigned int sleep(unsigned int seconds) {
    syscall(13, seconds * 1000);  /* SYS_SLEEP in ms */
    return 0;
}

void _exit(int status) {
    syscall(0, status);
    while (1);
}
