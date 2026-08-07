/*
 * Lestra OS - ext2 fd-shim layer (enhanced with directory support)
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * The existing ext2 driver in kernel/fs/ext2/ext2.c is path-based:
 *   ext2_read_file(path, buf, bufsize)  -- one-shot, reads the whole file
 *   ext2_list_dir(path, callback)       -- lists entries of any directory
 *   ext2_get_inode_mode(path)           -- returns i_mode (dir/file/etc.)
 *
 * VFS expects an fd-based interface:
 *   vfs_open(path) -> fd
 *   vfs_read(fd, buf, count) -> n
 *   vfs_close(fd) -> 0
 *   vfs_readdir(fd, entry) -> 0/-1
 *
 * This file bridges the two. It maintains a small table of open ext2
 * "files" (which may be regular files or directories). For regular
 * files, the full contents are cached in memory. For directories,
 * the listing is cached as an array of dirent entries.
 *
 * ext2_get_inode_mode() is used to determine whether a path is a
 * directory before we attempt to open it, avoiding the ambiguity
 * where ext2_read_file returns 0 for both "not found" and "directory".
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/vfs.h>
#include <string.h>

#include <lestra/ext2.h>

#define MAX_EXT2_FDS       16
#define EXT2_MAX_FILE      (256 * 1024)   /* 256 KB cap per open file (cache) */
#define EXT2_MAX_DIR_ENTRIES 128          /* max cached dir entries per fd */

/* ext2 inode type masks (same as in ext2.c) */
#define EXT2_S_IFDIR   0x4000
#define EXT2_S_IFREG   0x8000

struct ext2_dir_cache {
    int count;
    struct dirent entries[EXT2_MAX_DIR_ENTRIES];
};

struct ext2_open_file {
    int       used;
    int       is_dir;                     /* 1 = directory open, 0 = regular file */
    int       offset;                     /* read offset (regular) or readdir cursor (dir) */
    int       size;                       /* cached data size (regular files) */
    uint8_t*  data;                       /* kmalloc'd file data cache (regular files) */
    struct ext2_dir_cache* dir_cache;     /* kmalloc'd dir listing cache (directories) */
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

/* Callback wrappers for ext2_list_dir / ext2_list_root.
 * These populate a dir_cache structure via a thread-local pointer. */
static struct ext2_dir_cache* ext2_current_dir_cache = NULL;

static void ext2_dir_callback_compat(const char* name, uint32_t inode, uint8_t type) {
    if (!ext2_current_dir_cache) return;
    struct ext2_dir_cache* cache = ext2_current_dir_cache;
    if (cache->count >= EXT2_MAX_DIR_ENTRIES) return;
    struct dirent* e = &cache->entries[cache->count++];
    e->inode = inode;
    e->type  = type;
    e->reclen = sizeof(struct dirent);
    strncpy(e->name, name, MAX_NAME_LEN - 1);
    e->name[MAX_NAME_LEN - 1] = '\0';
}

/* Open a file or directory on ext2.
 *
 * First checks ext2_get_inode_mode() to determine the type:
 *   - If i_mode has EXT2_S_IFDIR (0x4000): it's a directory.
 *     We cache its listing via ext2_list_dir().
 *   - If i_mode has EXT2_S_IFREG (0x8000): it's a regular file.
 *     We read its full contents via ext2_read_file().
 *   - If ext2_get_inode_mode returns 0: path not found.
 *
 * Returns an fd index (0..MAX_EXT2_FDS-1) or -1 on failure. */
int ext2_open_file(const char* path) {
    if (!ext2_is_mounted() || !path) return -1;
    int slot = ext2_alloc_fd();
    if (slot < 0) {
        pr_warn("ext2-shim: no free fds\n");
        return -1;
    }

    /* Determine whether the path is a directory or regular file. */
    uint16_t mode = ext2_get_inode_mode(path);
    if (mode == 0) {
        /* Path not found on ext2. */
        return -1;
    }

    if ((mode & 0xF000) == EXT2_S_IFDIR) {
        /* Directory — cache the listing. */
        struct ext2_dir_cache* dc = (struct ext2_dir_cache*)kmalloc(sizeof(struct ext2_dir_cache));
        if (!dc) {
            pr_warn("ext2-shim: kmalloc for dir cache failed\n");
            return -1;
        }
        dc->count = 0;

        ext2_current_dir_cache = dc;
        ext2_list_dir(path, ext2_dir_callback_compat);
        ext2_current_dir_cache = NULL;

        ext2_fds[slot].used      = 1;
        ext2_fds[slot].is_dir    = 1;
        ext2_fds[slot].offset    = 0;
        ext2_fds[slot].size      = 0;
        ext2_fds[slot].data      = NULL;
        ext2_fds[slot].dir_cache = dc;
        strncpy(ext2_fds[slot].path, path, MAX_PATH_LEN - 1);
        ext2_fds[slot].path[MAX_PATH_LEN - 1] = '\0';

        pr_info("ext2-shim: opened dir '%s' (%d entries cached)\n", path, dc->count);
        return slot;
    }

    /* Regular file — read and cache the contents. */
    uint8_t* buf = (uint8_t*)kmalloc(EXT2_MAX_FILE);
    if (!buf) {
        pr_warn("ext2-shim: kmalloc %d failed\n", EXT2_MAX_FILE);
        return -1;
    }

    int n = ext2_read_file(path, buf, EXT2_MAX_FILE);
    if (n < 0) {
        /* Unexpected: ext2_read_file shouldn't return negative.
         * ext2_get_inode_mode confirmed it's a regular file,
         * but read failed. Clean up and fail. */
        kfree(buf);
        pr_warn("ext2-shim: read failed for '%s' (mode=0x%x)\n", path, mode);
        return -1;
    }

    ext2_fds[slot].used      = 1;
    ext2_fds[slot].is_dir    = 0;
    ext2_fds[slot].offset    = 0;
    ext2_fds[slot].size      = n;
    ext2_fds[slot].data      = buf;
    ext2_fds[slot].dir_cache = NULL;
    strncpy(ext2_fds[slot].path, path, MAX_PATH_LEN - 1);
    ext2_fds[slot].path[MAX_PATH_LEN - 1] = '\0';
    return slot;
}

/* Read up to `count` bytes from the open file at its current offset. */
int ext2_read_file_fd(int fd, void* buf, int count) {
    if (fd < 0 || fd >= MAX_EXT2_FDS || !ext2_fds[fd].used) return -1;
    if (ext2_fds[fd].is_dir) return -1;   /* can't read() a directory */
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
    if (ext2_fds[fd].dir_cache) kfree(ext2_fds[fd].dir_cache);
    ext2_fds[fd].used    = 0;
    ext2_fds[fd].data    = NULL;
    ext2_fds[fd].dir_cache = NULL;
    ext2_fds[fd].offset  = 0;
    ext2_fds[fd].size    = 0;
    ext2_fds[fd].is_dir  = 0;
    return 0;
}

/* Readdir: return one entry from the cached directory listing.
 *
 * For a valid ext2 directory fd (is_dir=1 with dir_cache):
 *   - entry->inode is used as a cursor (index into dir_cache).
 *   - Returns 0 on success, -1 on end-of-directory or error.
 *
 * For legacy callers that don't open a directory first
 * (e.g. old shell code using fd=0):
 *   - Falls back to listing ext2 root. */
int ext2_readdir(int fd, struct dirent* entry) {
    if (!entry) return -1;

    /* If fd is a valid ext2 directory, use its cached listing. */
    if (fd >= 0 && fd < MAX_EXT2_FDS && ext2_fds[fd].used
        && ext2_fds[fd].is_dir && ext2_fds[fd].dir_cache) {
        struct ext2_dir_cache* dc = ext2_fds[fd].dir_cache;
        int idx = entry->inode;   /* cursor */
        if (idx < 0 || idx >= dc->count) return -1;  /* end of dir */
        *entry = dc->entries[idx];
        entry->inode = idx + 1;   /* next call starts here */
        return 0;
    }

    /* Legacy fallback: list ext2 root for callers that don't have
     * a proper directory fd. This is initialized once and cached. */
    static int ext2_legacy_readdir_initialized = 0;
    static struct ext2_dir_cache ext2_legacy_cache;
    if (!ext2_legacy_readdir_initialized) {
        ext2_legacy_cache.count = 0;
        ext2_current_dir_cache = &ext2_legacy_cache;
        ext2_list_root(ext2_dir_callback_compat);
        ext2_current_dir_cache = NULL;
        ext2_legacy_readdir_initialized = 1;
    }
    int idx = entry->inode;
    if (idx < 0 || idx >= ext2_legacy_cache.count) return -1;
    *entry = ext2_legacy_cache.entries[idx];
    entry->inode = idx + 1;
    return 0;
}

/* Stat: determine file/dir type via ext2_get_inode_mode, then
 * open the path to get the size, close it. */
int ext2_stat_file(const char* path, struct stat* st) {
    if (!path || !st) return -1;

    /* Check if path exists and what type it is. */
    uint16_t mode = ext2_get_inode_mode(path);
    if (mode == 0) return -1;   /* not found */

    memset(st, 0, sizeof(*st));

    if ((mode & 0xF000) == EXT2_S_IFDIR) {
        st->mode = S_IFDIR | 0755;
        st->size = 0;
        return 0;
    }

    /* Regular file: open to get the size. */
    int fd = ext2_open_file(path);
    if (fd < 0) return -1;

    st->mode = S_IFREG | 0644;
    st->size = (uint64_t)ext2_fds[fd].size;
    ext2_close_file(fd);
    return 0;
}

/* Return the cached file size for an open ext2 fd.
 * Used by vfs_lseek SEEK_END to compute offsets. */
int ext2_file_size(int fd) {
    if (fd < 0 || fd >= MAX_EXT2_FDS || !ext2_fds[fd].used) return -1;
    if (ext2_fds[fd].is_dir) return -1;   /* directories don't have a meaningful "file size" */
    return ext2_fds[fd].size;
}
