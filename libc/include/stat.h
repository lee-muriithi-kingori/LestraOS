/*
 * Lestra OS - C Standard Library - stat (POSIX)
 * Copyright (c) 2026 lestramk.org
 *
 * W1-A finding F: previously the libc had no <stat.h>, so programs
 * that needed struct stat / S_IFREG / S_ISDIR had to hand-define
 * them.  The kernel's sys_stat / sys_fstat already fill a struct
 * stat — this header mirrors the layout so libc callers see the
 * same fields the kernel writes.
 *
 * Field widths and offsets match the kernel's struct stat in
 * kernel/syscall/syscall.c (sys_stat/sys_fstat fill the same
 * fields).  If the kernel layout changes, update this header to
 * match.
 *
 * Freestanding — no FP, no SSE.
 */
#ifndef LIBC_STAT_H
#define LIBC_STAT_H

#include <stddef.h>
#include <stdint.h>

/* File-statistic structure.  Match the kernel's layout exactly so
 * the libc user sees the bytes the kernel wrote.  (The kernel uses
 * the same field order/names.) */
struct stat {
    uint64_t st_dev;        /* device file is on */
    uint64_t st_ino;        /* inode number */
    uint32_t st_mode;       /* file type + permissions */
    uint32_t st_nlink;      /* number of hard links */
    uint32_t st_uid;        /* owner user ID */
    uint32_t st_gid;        /* owner group ID */
    uint64_t st_rdev;       /* device type (if inode is a device) */
    uint64_t st_size;       /* file size in bytes */
    uint64_t st_blksize;    /* preferred I/O block size */
    uint64_t st_blocks;     /* number of 512-byte blocks allocated */
    uint64_t st_atime;      /* time of last access (seconds) */
    uint64_t st_mtime;      /* time of last modification (seconds) */
    uint64_t st_ctime;      /* time of last status change (seconds) */
};

/* File type bits in st_mode (masks the file-type field). */
#define S_IFMT    0037000   /* mask for file type bits */
#define S_IFSOCK  0140000   /* socket */
#define S_IFLNK   0120000   /* symbolic link */
#define S_IFREG   0100000   /* regular file */
#define S_IFBLK   0060000   /* block device */
#define S_IFDIR   0040000   /* directory */
#define S_IFCHR   0020000   /* character device */
#define S_IFIFO   0010000   /* FIFO (named pipe) */

/* File-type predicates. */
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* Permission bits in st_mode (low 12 bits on Linux; POSIX). */
#define S_ISUID  0004000   /* set-user-ID on execution */
#define S_ISGID  0002000   /* set-group-ID on execution */
#define S_ISVTX  0001000   /* sticky bit */
#define S_IRWXU  0000700   /* owner: rwx */
#define S_IRUSR  0000400   /* owner: read */
#define S_IWUSR  0000200   /* owner: write */
#define S_IXUSR  0000100   /* owner: execute */
#define S_IRWXG  0000070   /* group: rwx */
#define S_IRGRP  0000040   /* group: read */
#define S_IWGRP  0000020   /* group: write */
#define S_IXGRP  0000010   /* group: execute */
#define S_IRWXO  0000007   /* others: rwx */
#define S_IROTH  0000004   /* others: read */
#define S_IWOTH  0000002   /* others: write */
#define S_IXOTH  0000001   /* others: execute */

/* Wrappers (defined in libc/src/unistd.c).  The kernel implements
 * sys_stat(18) and sys_fstat(31); sys_lstat does not exist (W1-A
 * finding H), so lstat() currently delegates to stat() — the kernel
 * has no symlink-following VFS yet. */
int stat(const char* pathname, struct stat* st);
int fstat(int fd, struct stat* st);
int lstat(const char* pathname, struct stat* st);

#endif /* LIBC_STAT_H */
