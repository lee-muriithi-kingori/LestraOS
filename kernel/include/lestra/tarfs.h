#ifndef LESTRA_TARFS_H
#define LESTRA_TARFS_H

#include <lestra/types.h>
#include <lestra/vfs.h>

void tarfs_init(const void* blob, uint64_t size);
int tarfs_open(const char* path);
int tarfs_close(int fd);
ssize_t tarfs_read(int fd, void* buf, size_t count);
int tarfs_stat(const char* path, struct stat* st);
int tarfs_is_mounted(void);
int tarfs_file_count(void);
int tarfs_resolve_symlink(const char* path, char* resolved, size_t rsz);
int tarfs_readlink(const char* path, char* buf, size_t bufsz);

#define TARFS_FD_BASE 200

#endif
