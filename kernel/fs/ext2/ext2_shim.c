/*
 * Lestra OS - ext2 fd-shim layer
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * The existing ext2 driver in kernel/fs/ext2/ext2.c is path-based:
 *   ext2_read_file(path, buf, bufsize)  -- one-shot, reads the whole file
 *
 * VFS expects an fd-based interface:
 *   vfs_open(path) -> fd
 *   vfs_read(fd, buf, count) -> n
 *   vfs_close(fd) -> 0
 *
 * This file bridges the two. It maintains a small table of open ext2
 * files, each with the full file contents cached in memory (since
 * ext2_read_file is one-shot). For large files this is wasteful, but
 * it's correct and unblocks VFS plumbing today. A future refactor
 * could make ext2_read_file incremental.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/vfs.h>
#include <string.h>

/* ext2 driver entry points (from kernel/fs/ext2/ext2.c). */
extern int  ext2_is_mounted(void);
extern int  ext2_read_file(const char* path, void* buf, uint32_t bufsize);
extern void ext2_list_root(void (*callback)(const char* name, uint32_t inode, uint8_t type));
extern uint32_t ext2_resolve_path(const char* path);

#define MAX_EXT2_FDS  16
#define EXT2_MAX_FILE  (256 * 1024)   /* 256 KB cap per open file (cache) */

struct ext2_open_file {
    int       used;
    int       offset;
    int       size;
    uint8_t*  data;
    char      path[MAX_PATH_LEN];
};

static struct ext2_open_file ext2_fds[MAX_EXT2_FDS];

/* Allocate a free ext2 fd slot. Returns index 0..MAX_EXT2_FDS-1 or -1. */
static int ext2_alloc_fd(void) {
    for (int i = 0; i < MAX_EXT2_FDS; i++) {
        if (!ext2_fds[i].used) return i;
    }
    return -1;
}

/* Open a file on ext2: read its full contents into a heap cache,
 * return an fd index. */
int ext2_open_file(const char* path) {
    if (!ext2_is_mounted() || !path) return -1;
    int slot = ext2_alloc_fd();
    if (slot < 0) {
        pr_warn("ext2-shim: no free fds\n");
        return -1;
    }

    /* Allocate cache buffer. */
    uint8_t* buf = (uint8_t*)kmalloc(EXT2_MAX_FILE);
    if (!buf) {
        pr_warn("ext2-shim: kmalloc %d failed\n", EXT2_MAX_FILE);
        return -1;
    }

    int n = ext2_read_file(path, buf, EXT2_MAX_FILE);
    if (n < 0) {
        kfree(buf);
        return -1;
    }

    ext2_fds[slot].used   = 1;
    ext2_fds[slot].offset = 0;
    ext2_fds[slot].size   = n;
    ext2_fds[slot].data   = buf;
    strncpy(ext2_fds[slot].path, path, MAX_PATH_LEN - 1);
    ext2_fds[slot].path[MAX_PATH_LEN - 1] = '\0';
    return slot;
}

/* Read up to `count` bytes from the open file at its current offset. */
int ext2_read_file_fd(int fd, void* buf, int count) {
    if (fd < 0 || fd >= MAX_EXT2_FDS || !ext2_fds[fd].used) return -1;
    if (!buf || count <= 0) return 0;
    struct ext2_open_file* f = &ext2_fds[fd];
    int avail = f->size - f->offset;
    if (avail <= 0) return 0;
    if (count > avail) count = avail;
    memcpy(buf, f->data + f->offset, count);
    f->offset += count;
    return count;
}

/* VFS uses the name ext2_read_file(fd, buf, count) — alias.
 * Note: this SHADOWS the path-based ext2_read_file() in ext2.c when
 * both files are linked into the same binary. To avoid the conflict
 * we rename this to ext2_read_fd and have vfs.c call ext2_read_fd(). */
int ext2_read_fd(int fd, void* buf, int count) {
    return ext2_read_file_fd(fd, buf, count);
}

/* Close: free the cache, mark slot free. */
int ext2_close_file(int fd) {
    if (fd < 0 || fd >= MAX_EXT2_FDS || !ext2_fds[fd].used) return -1;
    if (ext2_fds[fd].data) kfree(ext2_fds[fd].data);
    ext2_fds[fd].used   = 0;
    ext2_fds[fd].data   = NULL;
    ext2_fds[fd].offset = 0;
    ext2_fds[fd].size   = 0;
    return 0;
}

/* Readdir: not really supported per-directory in the existing ext2
 * driver; we only have ext2_list_root which dumps root. We implement
 * a minimal version that returns the root listing. */
static int ext2_readdir_cursor = 0;
static struct dirent ext2_readdir_buf[MAX_OPEN_FILES];   /* small cache */
static int ext2_readdir_count = 0;
static int ext2_readdir_initialized = 0;

static void ext2_readdir_callback(const char* name, uint32_t inode, uint8_t type) {
    if (ext2_readdir_count >= MAX_OPEN_FILES) return;
    struct dirent* e = &ext2_readdir_buf[ext2_readdir_count++];
    e->inode = inode;
    e->type = type;
    e->reclen = sizeof(struct dirent);
    strncpy(e->name, name, MAX_NAME_LEN - 1);
    e->name[MAX_NAME_LEN - 1] = '\0';
}

int ext2_readdir(int fd, struct dirent* entry) {
    (void)fd;   /* the existing ext2 driver doesn't track dir fds */
    if (!entry) return -1;
    if (!ext2_readdir_initialized) {
        ext2_list_root(ext2_readdir_callback);
        ext2_readdir_initialized = 1;
    }
    /* entry->inode is used as the cursor (matches memfs convention). */
    int idx = entry->inode;
    if (idx < 0 || idx >= ext2_readdir_count) return -1;
    *entry = ext2_readdir_buf[idx];
    entry->inode = idx + 1;   /* next call */
    return 0;
}

/* Stat: read the file size by calling ext2_read_file with a NULL buf
 * and bufsize 0... actually ext2_read_file doesn't support that, so
 * we just open the file, get its size, close it. */
int ext2_stat_file(const char* path, struct stat* st) {
    if (!path || !st) return -1;
    int fd = ext2_open_file(path);
    if (fd < 0) return -1;
    memset(st, 0, sizeof(*st));
    st->size = ext2_fds[fd].size;
    st->mode = 0644;
    ext2_close_file(fd);
    return 0;
}
