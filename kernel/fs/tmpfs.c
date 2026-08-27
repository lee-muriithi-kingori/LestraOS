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

#include <lestra/types.h>
#include <lestra/tmpfs.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/vfs.h>   /* for O_CREAT / O_RDWR / O_APPEND / O_TRUNC */
#include <string.h>

struct tmpfs_inode {
    int   used;
    char  name[64];
    size_t size;
    uint8_t* data;     /* kmalloc'd, size == TMPFS_MAX_FILE_SIZE */
    int   refcount;
};

struct tmpfs_open {
    int   used;
    int   inode_idx;   /* -1 for directory handle */
    size_t pos;
    int   flags;
    int   is_dir;
};

static struct tmpfs_inode tmpfs_inodes[TMPFS_MAX_FILES];
static struct tmpfs_open  tmpfs_opens[TMPFS_MAX_OPEN];

int tmpfs_is_tmpfs_fd(int fd) {
    return fd >= TMPFS_FD_BASE && fd < TMPFS_FD_BASE + TMPFS_MAX_OPEN;
}

static int find_inode(const char* path) {
    if (!path) return -1;
    /* Accept both /tmp/foo and foo (relative to /tmp). */
    const char* p = path;
    if (strncmp(p, "/tmp/", 5) == 0) p += 5;
    else if (strcmp(p, "/tmp") == 0) return -1;
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (tmpfs_inodes[i].used && strcmp(tmpfs_inodes[i].name, p) == 0) {
            return i;
        }
    }
    return -1;
}

static int alloc_inode(const char* path) {
    const char* p = path;
    if (strncmp(p, "/tmp/", 5) == 0) p += 5;
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!tmpfs_inodes[i].used) {
            tmpfs_inodes[i].used = 1;
            tmpfs_inodes[i].refcount = 0;
            tmpfs_inodes[i].size = 0;
            strncpy(tmpfs_inodes[i].name, p, sizeof(tmpfs_inodes[i].name) - 1);
            tmpfs_inodes[i].name[sizeof(tmpfs_inodes[i].name) - 1] = '\0';
            tmpfs_inodes[i].data = (uint8_t*)kmalloc(TMPFS_MAX_FILE_SIZE);
            if (!tmpfs_inodes[i].data) {
                tmpfs_inodes[i].used = 0;
                return -1;
            }
            return i;
        }
    }
    return -1;
}

int tmpfs_open(const char* path, int flags) {
    /* Directory open for /tmp itself */
    if (path && (strcmp(path, "/tmp") == 0 || strcmp(path, "/tmp/") == 0)) {
        for (int i = 0; i < TMPFS_MAX_OPEN; i++) {
            if (!tmpfs_opens[i].used) {
                tmpfs_opens[i].used = 1;
                tmpfs_opens[i].inode_idx = -1;
                tmpfs_opens[i].pos = 0;
                tmpfs_opens[i].flags = flags;
                tmpfs_opens[i].is_dir = 1;
                return i + TMPFS_FD_BASE;
            }
        }
        return -1;
    }
    int idx = find_inode(path);
    if (idx < 0 && (flags & O_CREAT)) {
        idx = alloc_inode(path);
        if (idx < 0) return -1;
    }
    if (idx < 0) return -1;

    /* O_TRUNC shrinks the file to zero. */
    if ((flags & O_TRUNC) && tmpfs_inodes[idx].data) {
        tmpfs_inodes[idx].size = 0;
    }

    for (int i = 0; i < TMPFS_MAX_OPEN; i++) {
        if (!tmpfs_opens[i].used) {
            tmpfs_opens[i].used = 1;
            tmpfs_opens[i].inode_idx = idx;
            tmpfs_opens[i].pos = (flags & O_APPEND) ? tmpfs_inodes[idx].size : 0;
            tmpfs_opens[i].flags = flags;
            tmpfs_opens[i].is_dir = 0;
            tmpfs_inodes[idx].refcount++;
            return i + TMPFS_FD_BASE;
        }
    }
    return -1;  /* table full */
}

int tmpfs_close(int fd) {
    fd -= TMPFS_FD_BASE;
    if (fd < 0 || fd >= TMPFS_MAX_OPEN || !tmpfs_opens[fd].used) return -1;
    if (tmpfs_opens[fd].is_dir) {
        tmpfs_opens[fd].used = 0;
        tmpfs_opens[fd].is_dir = 0;
        return 0;
    }
    int idx = tmpfs_opens[fd].inode_idx;
    if (idx >= 0 && idx < TMPFS_MAX_FILES && tmpfs_inodes[idx].used) {
        tmpfs_inodes[idx].refcount--;
        /* Don't free inodes on close — files persist until reboot or
         * unlink (which we don't implement yet). Just decrement. */
    }
    tmpfs_opens[fd].used = 0;
    tmpfs_opens[fd].is_dir = 0;
    return 0;
}

ssize_t tmpfs_read(int fd, void* buf, size_t count) {
    fd -= TMPFS_FD_BASE;
    if (fd < 0 || fd >= TMPFS_MAX_OPEN || !tmpfs_opens[fd].used) return -1;
    if (!buf) return -EFAULT;
    int idx = tmpfs_opens[fd].inode_idx;
    if (idx < 0 || !tmpfs_inodes[idx].used) return -1;
    struct tmpfs_inode* ino = &tmpfs_inodes[idx];
    if (tmpfs_opens[fd].pos >= ino->size) return 0;
    size_t avail = ino->size - tmpfs_opens[fd].pos;
    if (count > avail) count = avail;
    memcpy(buf, ino->data + tmpfs_opens[fd].pos, count);
    tmpfs_opens[fd].pos += count;
    return (ssize_t)count;
}

ssize_t tmpfs_write(int fd, const void* buf, size_t count) {
    fd -= TMPFS_FD_BASE;
    if (fd < 0 || fd >= TMPFS_MAX_OPEN || !tmpfs_opens[fd].used) return -1;
    if (!buf) return -EFAULT;
    int idx = tmpfs_opens[fd].inode_idx;
    if (idx < 0 || !tmpfs_inodes[idx].used) return -1;
    struct tmpfs_inode* ino = &tmpfs_inodes[idx];
    if (tmpfs_opens[fd].flags & O_APPEND) {
        tmpfs_opens[fd].pos = ino->size;
    }
    if (tmpfs_opens[fd].pos + count > TMPFS_MAX_FILE_SIZE) {
        count = (TMPFS_MAX_FILE_SIZE > tmpfs_opens[fd].pos)
                ? (TMPFS_MAX_FILE_SIZE - tmpfs_opens[fd].pos) : 0;
    }
    if (count == 0) return 0;
    memcpy(ino->data + tmpfs_opens[fd].pos, buf, count);
    tmpfs_opens[fd].pos += count;
    if (tmpfs_opens[fd].pos > ino->size) ino->size = tmpfs_opens[fd].pos;
    return (ssize_t)count;
}

int tmpfs_lseek(int fd, off_t offset, int whence) {
    fd -= TMPFS_FD_BASE;
    if (fd < 0 || fd >= TMPFS_MAX_OPEN || !tmpfs_opens[fd].used) return -1;
    int idx = tmpfs_opens[fd].inode_idx;
    if (idx < 0 || !tmpfs_inodes[idx].used) return -1;
    off_t base;
    switch (whence) {
        case 0: base = 0; break;
        case 1: base = (off_t)tmpfs_opens[fd].pos; break;
        case 2: base = (off_t)tmpfs_inodes[idx].size; break;
        default: return -1;
    }
    off_t new_off = base + offset;
    if (new_off < 0) return -1;
    tmpfs_opens[fd].pos = (size_t)new_off;
    return (int)new_off;
}

int tmpfs_read_at(int fd, void* buf, size_t count, off_t offset) {
    fd -= TMPFS_FD_BASE;
    if (fd < 0 || fd >= TMPFS_MAX_OPEN || !tmpfs_opens[fd].used) return -1;
    if (!buf) return -EFAULT;
    int idx = tmpfs_opens[fd].inode_idx;
    if (idx < 0 || !tmpfs_inodes[idx].used) return -1;
    struct tmpfs_inode* ino = &tmpfs_inodes[idx];
    if ((size_t)offset >= ino->size) return 0;
    size_t avail = ino->size - (size_t)offset;
    if (count > avail) count = avail;
    memcpy(buf, ino->data + (size_t)offset, count);
    return (int)count;
}

int tmpfs_unlink(const char* path) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    if (tmpfs_inodes[idx].refcount > 0) {
        /* File is still open — deny unlink or allow? For simplicity allow but mark */
        /* If open, we still free data but keep inode until close? Simpler: fail if open */
        return -1;
    }
    if (tmpfs_inodes[idx].data) kfree(tmpfs_inodes[idx].data);
    tmpfs_inodes[idx].data = NULL;
    tmpfs_inodes[idx].used = 0;
    tmpfs_inodes[idx].size = 0;
    tmpfs_inodes[idx].name[0] = '\0';
    return 0;
}

int tmpfs_stat(const char* path, struct stat* st) {
    if (!path || !st) return -1;
    /* /tmp directory itself */
    if (strcmp(path, "/tmp") == 0 || strcmp(path, "/tmp/") == 0) {
        memset(st, 0, sizeof(*st));
        st->mode = S_IFDIR | 0755;
        st->size = 0;
        return 0;
    }
    int idx = find_inode(path);
    if (idx < 0) return -1;
    memset(st, 0, sizeof(*st));
    st->mode = S_IFREG | 0644;
    st->size = tmpfs_inodes[idx].size;
    return 0;
}

int tmpfs_truncate(const char* path, off_t length) {
    if (!path) return -1;
    if (length < 0 || (uint64_t)length > TMPFS_MAX_FILE_SIZE) return -1;
    int idx = find_inode(path);
    if (idx < 0) return -1;
    if (tmpfs_inodes[idx].used == 0) return -1;
    size_t new_len = (size_t)length;
    size_t old_len = tmpfs_inodes[idx].size;
    if (new_len > old_len) {
        if (tmpfs_inodes[idx].data)
            memset(tmpfs_inodes[idx].data + old_len, 0, new_len - old_len);
    } else if (new_len < old_len) {
        if (tmpfs_inodes[idx].data)
            memset(tmpfs_inodes[idx].data + new_len, 0, old_len - new_len);
    }
    tmpfs_inodes[idx].size = new_len;
    for (int i = 0; i < TMPFS_MAX_OPEN; i++) {
        if (tmpfs_opens[i].used && tmpfs_opens[i].inode_idx == idx) {
            if (tmpfs_opens[i].pos > new_len) tmpfs_opens[i].pos = new_len;
        }
    }
    return 0;
}

int tmpfs_ftruncate(int fd, off_t length) {
    if (length < 0 || (uint64_t)length > TMPFS_MAX_FILE_SIZE) return -1;
    fd -= TMPFS_FD_BASE;
    if (fd < 0 || fd >= TMPFS_MAX_OPEN || !tmpfs_opens[fd].used) return -1;
    if (tmpfs_opens[fd].is_dir) return -1;
    int idx = tmpfs_opens[fd].inode_idx;
    if (idx < 0 || idx >= TMPFS_MAX_FILES || !tmpfs_inodes[idx].used) return -1;
    size_t new_len = (size_t)length;
    size_t old_len = tmpfs_inodes[idx].size;
    if (new_len > old_len) {
        if (tmpfs_inodes[idx].data)
            memset(tmpfs_inodes[idx].data + old_len, 0, new_len - old_len);
    } else if (new_len < old_len) {
        if (tmpfs_inodes[idx].data)
            memset(tmpfs_inodes[idx].data + new_len, 0, old_len - new_len);
    }
    tmpfs_inodes[idx].size = new_len;
    for (int i = 0; i < TMPFS_MAX_OPEN; i++) {
        if (tmpfs_opens[i].used && tmpfs_opens[i].inode_idx == idx) {
            if (tmpfs_opens[i].pos > new_len) tmpfs_opens[i].pos = new_len;
        }
    }
    return 0;
}

int tmpfs_readdir(int fd, struct dirent* entry) {
    if (!entry) return -1;
    int slot = fd - TMPFS_FD_BASE;
    if (slot < 0 || slot >= TMPFS_MAX_OPEN || !tmpfs_opens[slot].used) return -1;
    if (!tmpfs_opens[slot].is_dir) return -1;
    int cursor = (int)entry->inode;
    if (cursor < 0) return -1;
    int seen = 0;
    for (int i = 0; i < TMPFS_MAX_FILES; i++) {
        if (!tmpfs_inodes[i].used) continue;
        if (seen == cursor) {
            entry->inode = cursor + 1;
            entry->type = FT_REGULAR;
            entry->reclen = sizeof(struct dirent);
            strncpy(entry->name, tmpfs_inodes[i].name, MAX_NAME_LEN - 1);
            entry->name[MAX_NAME_LEN - 1] = '\0';
            return 0;
        }
        seen++;
    }
    return -1;
}

void tmpfs_init(void) {
    memset(tmpfs_inodes, 0, sizeof(tmpfs_inodes));
    memset(tmpfs_opens,  0, sizeof(tmpfs_opens));
    pr_info("tmpfs: initialized (FD range %d..%d, max %d files, %d bytes each)\n",
            TMPFS_FD_BASE, TMPFS_FD_BASE + TMPFS_MAX_OPEN - 1,
            TMPFS_MAX_FILES, TMPFS_MAX_FILE_SIZE);
}
