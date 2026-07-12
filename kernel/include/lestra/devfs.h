/*
 * Lestra OS - /dev filesystem
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Minimal /dev with the four device files every Unix user expects:
 *
 *   /dev/null      read returns 0 bytes; write discards everything
 *   /dev/zero      read returns count zero bytes; write discards
 *   /dev/urandom   read returns count pseudo-random bytes
 *   /dev/tty       open returns -1 (we don't have a controlling tty)
 *
 * FDs live in [400..499] so they don't collide with VFS, ext2, tarfs,
 * procfs, tmpfs, or sockets.
 */
#ifndef LESTRA_DEVFS_H
#define LESTRA_DEVFS_H

#include <lestra/types.h>

#define DEVFS_FD_BASE     400
#define DEVFS_MAX_OPEN     100   /* fds 400..499 */

void    devfs_init(void);
int     devfs_open(const char* path);
int     devfs_close(int fd);
ssize_t devfs_read(int fd, void* buf, size_t count);
ssize_t devfs_write(int fd, const void* buf, size_t count);
int     devfs_is_devfs_fd(int fd);

#endif /* LESTRA_DEVFS_H */
