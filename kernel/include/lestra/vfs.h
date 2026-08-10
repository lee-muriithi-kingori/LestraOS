/*
 * Lestra OS - Virtual File System
 * Copyright (c) 2026 lestramk.org
 */

#ifndef LESTRA_VFS_H
#define LESTRA_VFS_H

#include <stdint.h>
#include <lestra/types.h>

#ifndef _OFF_T_DEFINED
typedef int64_t off_t;
#define _OFF_T_DEFINED
#endif

#define MAX_PATH_LEN    256
#define MAX_NAME_LEN    64
#define MAX_OPEN_FILES  128
#define MAX_MOUNTS      8
#define MAX_CHILDREN    64    /* max children per memfs directory */

/* Filesystem type identifiers for mount table */
#define FS_TYPE_MEMFS   0
#define FS_TYPE_EXT2    1
#define FS_TYPE_FAT32   2

/* File types */
#define FT_REGULAR   1
#define FT_DIRECTORY 2
#define FT_CHARDEV   3
#define FT_BLOCKDEV  4
#define FT_PIPE      5
#define FT_SYMLINK   6
#define FT_SOCKET    7

/* Open flags */
#define O_RDONLY    0x0001
#define O_WRONLY    0x0002
#define O_RDWR      0x0003
#define O_CREAT     0x0010
#define O_TRUNC     0x0020
#define O_APPEND    0x0040
#define O_DIRECTORY 0x0100

/* File mode type masks (POSIX) */
#define S_IFMT   0170000   /* type mask */
#define S_IFDIR  0040000   /* directory */
#define S_IFREG  0100000   /* regular file */
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)

/* Permissions */
#define S_IRUSR     0400
#define S_IWUSR     0200
#define S_IXUSR     0100
#define S_IRGRP     0040
#define S_IWGRP     0020
#define S_IXGRP     0010
#define S_IROTH     0004
#define S_IWOTH     0002
#define S_IXOTH     0001

/* File stat */
struct stat {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
};

/* Directory entry */
struct dirent {
    uint32_t inode;
    uint16_t reclen;
    uint8_t  type;
    char     name[MAX_NAME_LEN];
};

/* File operations */
/* Forward declaration so file_ops can reference vnode* before vnode is
 * defined below. Without this, gcc warns "struct vnode declared inside
 * parameter list will not be visible outside of this definition". */
struct vnode;
struct file_ops {
    int (*open)(struct vnode* node, int flags);
    int (*close)(struct vnode* node);
    ssize_t (*read)(struct vnode* node, void* buf, size_t count, off_t offset);
    ssize_t (*write)(struct vnode* node, const void* buf, size_t count, off_t offset);
    int (*readdir)(struct vnode* node, struct dirent* entry, int index);
};

/* VFS node (vnode) */
struct vnode {
    uint32_t inode;
    uint32_t refcount;
    uint32_t flags;
    uint16_t type;
    uint16_t mode;
    uint64_t size;
    struct file_ops* ops;
    void* private_data;
    struct vnode* parent;
    struct vnode* mount;
};

/* Mount point */
struct mount {
    char path[MAX_PATH_LEN];     /* mount point path (e.g. "/", "/mnt/disk") */
    int fs_type;                  /* FS_TYPE_MEMFS or FS_TYPE_EXT2 */
    struct vnode* root;
    struct filesystem* fs;
    void* private_data;
};

/* Filesystem driver */
struct filesystem {
    char name[16];
    struct vnode* (*mount)(const char* source);
    int (*unmount)(struct mount* mount);
};

/* File descriptor */
struct file_descriptor {
    int fd;
    int flags;
    off_t offset;
    struct vnode* node;
};

/* VFS functions */
void vfs_init(void);
int vfs_mount(const char* source, const char* target, const char* fs_type);
int vfs_unmount(const char* path);
struct vnode* vfs_lookup(const char* path);
int vfs_resolve_path(const char* path);
int vfs_open(const char* path, int flags);
int vfs_close(int fd);
ssize_t vfs_read(int fd, void* buf, size_t count);
ssize_t vfs_write(int fd, const void* buf, size_t count);
ssize_t vfs_read_at(int fd, void* buf, size_t count, off_t offset);
ssize_t vfs_write_at(int fd, const void* buf, size_t count, off_t offset);
int vfs_readdir(int fd, struct dirent* entry);
int vfs_mkdir(const char* path, uint32_t mode);
int vfs_create(const char* path, uint32_t mode);
int vfs_stat(const char* path, struct stat* st);
off_t vfs_lseek(int fd, off_t offset, int whence);
int vfs_unlink(const char* path);
int vfs_chmod(const char* path, uint32_t mode);
int vfs_rmdir(const char* path);
int vfs_rename(const char* oldpath, const char* newpath);
void vfs_selftest(void);
void vfs_register_fs(struct filesystem* fs);
struct mount* vfs_get_mount(int idx);
int vfs_get_mount_count(void);

/* initrd */
void initrd_load(void* data, size_t size);
struct filesystem* initrd_get_fs(void);

#endif /* LESTRA_VFS_H */
