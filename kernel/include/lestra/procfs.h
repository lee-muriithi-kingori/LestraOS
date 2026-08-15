/*
 * Lestra OS - /proc filesystem
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Synthetic process-information filesystem modeled on Linux's /proc.
 * Provides:
 *   /proc/self/exe        - symlink to current process's executable
 *   /proc/self/maps       - memory map (one line per PT_LOAD region)
 *   /proc/self/auxv       - auxiliary vector (raw bytes, like Linux)
 *   /proc/self/cmdline    - NUL-separated argv
 *   /proc/meminfo         - kernel memory stats (MemTotal / MemFree / ...)
 *   /proc/cpuinfo         - real CPU info via CPUID
 *   /proc/version         - "LestraOS 1.0.0-alpha ... lestramk.org"
 *
 * All files are generated on demand at read time so they always
 * reflect current state. FDs live in the range [PROCFS_FD_BASE,
 * PROCFS_FD_BASE + PROCFS_MAX_OPEN).
 */
#ifndef LESTRA_PROCFS_H
#define LESTRA_PROCFS_H

#include <lestra/types.h>
#include <lestra/vfs.h>

#define PROCFS_FD_BASE    300
#define PROCFS_MAX_OPEN   100   /* fds 300..399 */

/* Lifecycle */
void procfs_init(void);

/* Per-file operations. Returns a fd in [300..399] on success, -1 on
 * failure (unknown path or fd table full). */
int     procfs_open(const char* path);
int     procfs_close(int fd);

/* Read up to `count` bytes into `buf` from the open procfs file.
 * Returns number of bytes read (0 at EOF), or -1 on bad fd. */
ssize_t procfs_read(int fd, void* buf, size_t count);

/* True if `fd` was handed out by procfs_open(). */
int     procfs_is_procfs_fd(int fd);

#endif /* LESTRA_PROCFS_H */
