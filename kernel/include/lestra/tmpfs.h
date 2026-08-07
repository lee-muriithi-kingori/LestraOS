/*
 * Lestra OS - /tmp filesystem
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Volatile in-memory filesystem at /tmp. Supports up to TMPFS_MAX_FILES
 * files, each up to TMPFS_MAX_FILE_SIZE bytes. Data is held in plain
 * kmalloc'd buffers and is lost on reboot.
 *
 * FDs live in [500..599] so they don't collide with VFS (3..66),
 * ext2 (100..199), tarfs (200..299), procfs (300..399), devfs
 * (400..499), or sockets (600..631).
 */
#ifndef LESTRA_TMPFS_H
#define LESTRA_TMPFS_H

#include <lestra/types.h>

#define TMPFS_FD_BASE        500
#define TMPFS_MAX_OPEN        100   /* fds 500..599 */
#define TMPFS_MAX_FILES        64   /* max distinct files */
#define TMPFS_MAX_FILE_SIZE  (64 * 1024)   /* 64 KB per file */

void    tmpfs_init(void);
int     tmpfs_open(const char* path, int flags);
int     tmpfs_close(int fd);
ssize_t tmpfs_read(int fd, void* buf, size_t count);
ssize_t tmpfs_write(int fd, const void* buf, size_t count);
int     tmpfs_is_tmpfs_fd(int fd);

#endif /* LESTRA_TMPFS_H */
