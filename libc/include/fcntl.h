/*
 * Lestra OS - C Standard Library - fcntl (POSIX)
 * Copyright (c) 2026 lestramk.org
 *
 * W1-A finding F: previously the libc had no <fcntl.h>, so every
 * program that wanted O_CREAT / O_APPEND / O_NONBLOCK had to hand-
 * mirror the constants (the shell does this at user/shell/shell.c:26).
 *
 * Values match the Linux x86_64 ABI so unmodified Linux ELFs that
 * #include <fcntl.h> see the same bit patterns.
 *
 * NOTE: the kernel's sys_open(path, flags) currently only honours a
 * subset of these (O_RDONLY/O_WRONLY/O_RDWR/O_APPEND).  O_CREAT /
 * O_TRUNC / O_EXCL are accepted by the syscall but the underlying
 * VFS may not implement all of them yet.  Programs that pass O_CREAT
 * with a third `mode` argument still link cleanly — libc's open() is
 * variadic and ignores the extra argument.
 */
#ifndef LIBC_FCNTL_H
#define LIBC_FCNTL_H

#include <stdint.h>

/* open() access mode (mutually exclusive; low two bits). */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_ACCMODE 3

/* open() flags (Linux x86_64 values). */
#define O_CREAT       0100      /* create file if it doesn't exist */
#define O_EXCL        0200      /* fail if O_CREAT and file exists */
#define O_NOCTTY      0400      /* do not assign controlling tty */
#define O_TRUNC      01000      /* truncate file to zero length */
#define O_APPEND     02000      /* writes append to end */
#define O_NONBLOCK   04000      /* non-blocking I/O */
#define O_DSYNC     010000      /* write I/O data integrity */
#define O_DIRECT    040000      /* direct I/O (no buffering) */
#define O_LARGEFILE 0100000     /* large file support (always on x86_64) */
#define O_DIRECTORY 0200000     /* fail if not a directory */
#define O_NOFOLLOW  0400000     /* do not follow symlinks */
#define O_NOATIME  01000000     /* do not update atime on read */
#define O_CLOEXEC  02000000     /* set FD_CLOEXEC on the new fd */

/* fcntl() commands. */
#define F_DUPFD         0       /* duplicate fd (lowest >= arg) */
#define F_GETFD         1       /* read fd flags (FD_CLOEXEC) */
#define F_SETFD         2       /* set fd flags */
#define F_GETFL         3       /* read file status flags (O_*) */
#define F_SETFL         4       /* set file status flags */
#define F_GETLK         5
#define F_SETLK         6
#define F_SETLKW        7
#define F_SETOWN        8
#define F_GETOWN        9
#define F_SETSIG       10
#define F_GETSIG       11
#define F_SETOWN_EX    15
#define F_GETOWN_EX    16
#define F_OFD_GETLK    36
#define F_OFD_SETLK    37
#define F_OFD_SETLKW   38
#define F_SETLEASE     1024
#define F_GETLEASE     1025
#define F_NOTIFY       1026
#define F_DUPFD_CLOEXEC 1030

/* fd flags (returned/set by F_GETFD/F_SETFD). */
#define FD_CLOEXEC  1

/* fcntl() — wrapper defined in libc/src/unistd.c.  The kernel's
 * sys_fcntl takes (fd, cmd, arg); the libc wrapper accepts the
 * optional arg via varargs and forwards it.  Common commands:
 * F_DUPFD, F_GETFD/F_SETFD (close-on-exec), F_GETFL/F_SETFL. */
int fcntl(int fd, int cmd, ...);

#endif /* LIBC_FCNTL_H */
