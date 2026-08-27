/*
 * Lestra OS - FAT32 fd-shim layer (VFS bridge for read/write)
 * Copyright (c) 2026 lestramk.org
 *
 * Bridges the path-based FAT32 driver to the fd-based VFS interface.
 * Follows the same pattern as ext2_shim.c but adds write support.
 *
 * FD range: 700..715 (fat32 fd = actual fd - 700)
 *
 * Write strategy:
 *   - File data is cached in memory on open (like ext2_shim).
 *   - Writes modify the in-memory cache.
 *   - On close, if the cache was modified (dirty), the entire file
 *     is written back to FAT32 via fat32_write_file.
 *   - This is simple and correct for the small files lestraOS handles.
 *   - For large files, a page-cache approach would be better,
 *     but that's a future optimization.
 */

#include <lestra/types.h>
#include <lestra/fat32.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/vfs.h>
#include <string.h>

#define MAX_FAT32_FDS       16
#define FAT32_MAX_FILE      (64 * 1024)   /* 64 KB cache per open file */
#define FAT32_MAX_DIR_ENTRIES 128

struct fat32_dir_cache {
    int count;
    struct dirent entries[FAT32_MAX_DIR_ENTRIES];
};

struct fat32_open_file {
    int       used;
    int       is_dir;
    int       offset;         /* read/write offset */
    int       size;           /* cached data size */
    int       dirty;          /* 1 if cache was written to */
    uint32_t  first_cluster;  /* on-disk starting cluster (for write-back) */
    uint32_t  dir_cluster;    /* parent directory cluster (for subdir write-back) */
    uint8_t*  data;           /* kmalloc'd file data cache */
    struct fat32_dir_cache* dir_cache;
    char      name83[13];     /* 8.3 name for write-back */
    char      full_path[MAX_PATH_LEN]; /* original path for debugging */
};

static struct fat32_open_file fat32_fds[MAX_FAT32_FDS];

static int fat32_alloc_fd(void) {
    for (int i = 0; i < MAX_FAT32_FDS; i++) {
        if (!fat32_fds[i].used) return i;
    }
    return -1;
}

static void fat32_dir_callback(const char* name, uint32_t inode, uint8_t type) {
    /* We don't use the ext2-style callback — fat32_list_root returns
     * an array directly. This is a placeholder for the interface. */
    (void)name; (void)inode; (void)type;
}

/* Helper: get root directory cluster */
static uint32_t fat32_get_root_cluster(void) {
    const struct fat32_bpb *bpb = fat32_get_bpb();
    return bpb ? bpb->root_cluster : 2;
}

/* Resolve a directory path to its cluster.
 * dir_path is like "/", "/DIR", "/DIR/SUB".
 * Returns 0 on success, -1 on failure. */
static int resolve_cluster_for_dir(const char* dir_path, uint32_t *out_cluster) {
    if (!dir_path || !out_cluster) return -1;
    if (strcmp(dir_path, "/") == 0 || strcmp(dir_path, "") == 0) {
        *out_cluster = fat32_get_root_cluster();
        return 0;
    }
    char tmp[MAX_PATH_LEN];
    strncpy(tmp, dir_path, MAX_PATH_LEN - 1);
    tmp[MAX_PATH_LEN - 1] = '\0';
    char *p = tmp;
    while (*p == '/') p++;
    if (*p == '\0') {
        *out_cluster = fat32_get_root_cluster();
        return 0;
    }
    uint32_t cur = fat32_get_root_cluster();
    char *tok = p;
    while (tok && *tok) {
        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';
        if (*tok != '\0') {
            struct fat32_dirent de;
            if (fat32_lookup_in_dir(cur, tok, &de) < 0) return -1;
            if (!de.is_dir) return -1;
            if (de.first_cluster < 2) return -1;
            cur = de.first_cluster;
        }
        if (!slash) break;
        tok = slash + 1;
        while (*tok == '/') tok++;
        if (*tok == '\0') break;
    }
    *out_cluster = cur;
    return 0;
}

/* Resolve path to parent directory cluster and final 8.3 name.
 * For "/A/B/FILE.TXT": parent = cluster of "/A/B", name = "FILE.TXT"
 * For "/FILE.TXT": parent = root cluster, name = "FILE.TXT"
 * For "/": parent = root, name = "" (root itself)
 * Returns 0 on success, -1 on failure. */
static int resolve_parent_cluster(const char* path, uint32_t *out_parent, char *out_name) {
    if (!path || !out_parent || !out_name) return -1;
    out_name[0] = '\0';
    /* Handle root */
    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        *out_parent = fat32_get_root_cluster();
        return 0;
    }
    char tmp[MAX_PATH_LEN];
    strncpy(tmp, path, MAX_PATH_LEN - 1);
    tmp[MAX_PATH_LEN - 1] = '\0';
    /* Trim trailing slashes (except leading "/") */
    int len = strlen(tmp);
    while (len > 1 && tmp[len-1] == '/') {
        tmp[len-1] = '\0';
        len--;
    }
    char *last_slash = strrchr(tmp, '/');
    const char *name;
    char dir_path[MAX_PATH_LEN];
    if (!last_slash) {
        /* No slash — should not happen for absolute paths */
        strncpy(out_name, tmp, 12);
        out_name[12] = '\0';
        *out_parent = fat32_get_root_cluster();
        return 0;
    }
    name = last_slash + 1;
    strncpy(out_name, name, 12);
    out_name[12] = '\0';
    /* Extract parent dir path */
    int dir_len = (int)(last_slash - tmp);
    if (dir_len == 0) {
        strcpy(dir_path, "/");
    } else {
        if (dir_len >= MAX_PATH_LEN) dir_len = MAX_PATH_LEN - 1;
        memcpy(dir_path, tmp, dir_len);
        dir_path[dir_len] = '\0';
    }
    return resolve_cluster_for_dir(dir_path, out_parent);
}

/* Open a file or directory on FAT32.
 * For files: reads full contents into cache.
 * For directories: lists entries into cache.
 * Supports subdirectories via resolve_parent_cluster() + fat32_lookup_in_dir().
 * Returns fd index (0..MAX_FAT32_FDS-1) or -1. */
int fat32_shim_open(const char* path) {
    if (!fat32_is_mounted() || !path) return -1;

    /* Handle root directory explicitly */
    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        int slot = fat32_alloc_fd();
        if (slot < 0) {
            pr_warn("fat32-shim: no free fds\n");
            return -1;
        }
        struct fat32_open_file *f = &fat32_fds[slot];
        memset(f, 0, sizeof(*f));
        strncpy(f->name83, "/", 12);
        f->dir_cluster = fat32_get_root_cluster();
        strncpy(f->full_path, path, MAX_PATH_LEN - 1);
        struct fat32_dir_cache *dc = (struct fat32_dir_cache*)kmalloc(sizeof(struct fat32_dir_cache));
        if (!dc) { pr_warn("fat32-shim: kmalloc for dir cache failed\n"); return -1; }
        dc->count = 0;
        struct fat32_dirent entries[FAT32_MAX_DIR_ENTRIES];
        int count = fat32_list_dir(f->dir_cluster, entries, FAT32_MAX_DIR_ENTRIES);
        if (count < 0) count = fat32_list_root(entries, FAT32_MAX_DIR_ENTRIES);
        if (count > 0) {
            for (int i = 0; i < count && i < FAT32_MAX_DIR_ENTRIES; i++) {
                dc->entries[i].inode = entries[i].first_cluster;
                dc->entries[i].type = entries[i].is_dir ? FT_DIRECTORY : FT_REGULAR;
                dc->entries[i].reclen = sizeof(struct dirent);
                strncpy(dc->entries[i].name, entries[i].name, MAX_NAME_LEN - 1);
                dc->entries[i].name[MAX_NAME_LEN - 1] = '\0';
            }
            dc->count = count;
        }
        f->used = 1;
        f->is_dir = 1;
        f->dir_cache = dc;
        return slot;
    }

    char name83[13];
    uint32_t parent_cluster;
    if (resolve_parent_cluster(path, &parent_cluster, name83) < 0) {
        pr_warn("fat32-shim: failed to resolve parent for '%s'\n", path);
        return -1;
    }
    if (name83[0] == '\0') {
        pr_warn("fat32-shim: empty name for '%s'\n", path);
        return -1;
    }

    int slot = fat32_alloc_fd();
    if (slot < 0) {
        pr_warn("fat32-shim: no free fds\n");
        return -1;
    }

    /* Look up the file in its parent directory */
    struct fat32_dirent de;
    int found = fat32_lookup_in_dir(parent_cluster, name83, &de);

    if (found < 0 && !fat32_is_writable()) {
        /* Not found and can't create */
        return -1;
    }

    struct fat32_open_file *f = &fat32_fds[slot];
    memset(f, 0, sizeof(*f));
    strncpy(f->name83, name83, 12);
    f->name83[12] = '\0';
    f->dir_cluster = parent_cluster;
    strncpy(f->full_path, path, MAX_PATH_LEN - 1);
    f->full_path[MAX_PATH_LEN - 1] = '\0';

    if (found >= 0 && de.is_dir) {
        /* Directory — cache the listing */
        struct fat32_dir_cache *dc = (struct fat32_dir_cache*)kmalloc(sizeof(struct fat32_dir_cache));
        if (!dc) { pr_warn("fat32-shim: kmalloc for dir cache failed\n"); return -1; }
        dc->count = 0;

        /* Convert fat32_dirent array to dirent array */
        struct fat32_dirent entries[FAT32_MAX_DIR_ENTRIES];
        int count = fat32_list_dir(de.first_cluster, entries, FAT32_MAX_DIR_ENTRIES);
        if (count > 0) {
            for (int i = 0; i < count && i < FAT32_MAX_DIR_ENTRIES; i++) {
                dc->entries[i].inode = entries[i].first_cluster;
                dc->entries[i].type = entries[i].is_dir ? FT_DIRECTORY : FT_REGULAR;
                dc->entries[i].reclen = sizeof(struct dirent);
                strncpy(dc->entries[i].name, entries[i].name, MAX_NAME_LEN - 1);
                dc->entries[i].name[MAX_NAME_LEN - 1] = '\0';
            }
            dc->count = count;
        }

        f->used = 1;
        f->is_dir = 1;
        f->offset = 0;
        f->size = 0;
        f->data = NULL;
        f->dir_cache = dc;
        f->dirty = 0;
        f->first_cluster = de.first_cluster;
        return slot;
    }

    /* Regular file — read and cache contents */
    uint8_t *buf = (uint8_t *)kmalloc(FAT32_MAX_FILE);
    if (!buf) {
        pr_warn("fat32-shim: kmalloc %d failed\n", FAT32_MAX_FILE);
        return -1;
    }

    int file_size = 0;
    if (found >= 0) {
        file_size = fat32_read_file(de.first_cluster, de.file_size,
                                       buf, FAT32_MAX_FILE);
        if (file_size < 0) file_size = 0;
    }

    f->used = 1;
    f->is_dir = 0;
    f->offset = 0;
    f->size = file_size;
    f->data = buf;
    f->dir_cache = NULL;
    f->dirty = 0;
    f->first_cluster = found >= 0 ? de.first_cluster : 0;
    return slot;
}

/* Create a new file on FAT32 and open it.
 * Supports subdirectories via resolve_parent_cluster().
 * Returns fd index or -1. */
int fat32_shim_create(const char* path) {
    if (!fat32_is_writable() || !path) return -1;

    char name83[13];
    uint32_t parent_cluster;
    if (resolve_parent_cluster(path, &parent_cluster, name83) < 0) {
        pr_warn("fat32-shim: create resolve failed for '%s'\n", path);
        return -1;
    }
    if (name83[0] == '\0') {
        pr_warn("fat32-shim: empty name for create '%s'\n", path);
        return -1;
    }

    /* Create the file in the correct directory */
    struct fat32_dirent de;
    int rc;
    if (parent_cluster == fat32_get_root_cluster()) {
        rc = fat32_create_file(name83, &de);
    } else {
        rc = fat32_create_file_in_dir(parent_cluster, name83, &de);
    }
    if (rc < 0) {
        pr_warn("fat32-shim: create '%s' failed\n", name83);
        return -1;
    }

    /* Now open it */
    return fat32_shim_open(path);
}

int fat32_shim_read(int fd, void* buf, int count) {
    if (fd < 0 || fd >= MAX_FAT32_FDS || !fat32_fds[fd].used) return -1;
    if (fat32_fds[fd].is_dir) return -1;
    if (!buf || count <= 0) return 0;
    struct fat32_open_file *f = &fat32_fds[fd];
    int avail = f->size - f->offset;
    if (avail <= 0) return 0;
    if (count > avail) count = avail;
    memcpy(buf, f->data + f->offset, count);
    f->offset += count;
    return count;
}

int fat32_shim_write(int fd, const void* buf, int count) {
    if (fd < 0 || fd >= MAX_FAT32_FDS || !fat32_fds[fd].used) return -1;
    if (fat32_fds[fd].is_dir) return -1;
    if (!buf || count <= 0) return 0;
    struct fat32_open_file *f = &fat32_fds[fd];
    if (!fat32_is_writable()) return -1;

    int end = f->offset + count;
    if (end > FAT32_MAX_FILE) {
        count = FAT32_MAX_FILE - f->offset;
        if (count <= 0) return 0;
        end = FAT32_MAX_FILE;
    }

    memcpy(f->data + f->offset, buf, count);
    f->offset = end;
    if (end > f->size) f->size = end;
    f->dirty = 1;
    return count;
}

int fat32_shim_close(int fd) {
    if (fd < 0 || fd >= MAX_FAT32_FDS || !fat32_fds[fd].used) return -1;
    struct fat32_open_file *f = &fat32_fds[fd];

    /* Write back dirty data */
    if (f->dirty && !f->is_dir && fat32_is_writable()) {
        uint32_t new_cluster = f->first_cluster;
        uint32_t new_size = (uint32_t)f->size;
        int written = fat32_write_file(f->first_cluster, 0,
                                        f->data, (uint32_t)f->size, 0,
                                        &new_cluster, &new_size);
        if (written >= 0) {
            /* Update the directory entry's file size (handle subdirs) */
            int upd;
            if (f->dir_cluster == fat32_get_root_cluster()) {
                upd = fat32_update_entry(f->name83, new_cluster, new_size);
                if (upd < 0) upd = fat32_update_entry_size(f->name83, new_size);
            } else {
                upd = fat32_update_entry_in_dir(f->dir_cluster, f->name83, new_cluster, new_size);
            }
            if (upd == 0) {
                f->first_cluster = new_cluster;
                pr_info("fat32-shim: wrote back '%s' (%d bytes, cluster %u, dir %u)\n",
                        f->name83, new_size, new_cluster, f->dir_cluster);
            } else {
                pr_warn("fat32-shim: dir update failed for '%s'\n", f->name83);
            }
        } else {
            pr_warn("fat32-shim: write-back failed for '%s'\n", f->name83);
        }
    }

    if (f->data) kfree(f->data);
    if (f->dir_cache) kfree(f->dir_cache);
    f->used = 0;
    f->data = NULL;
    f->dir_cache = NULL;
    f->offset = 0;
    f->size = 0;
    f->is_dir = 0;
    f->dirty = 0;
    return 0;
}

int fat32_shim_readdir(int fd, struct dirent* entry) {
    if (!entry) return -1;
    if (fd < 0 || fd >= MAX_FAT32_FDS || !fat32_fds[fd].used) return -1;
    if (!fat32_fds[fd].is_dir || !fat32_fds[fd].dir_cache) return -1;

    struct fat32_dir_cache *dc = fat32_fds[fd].dir_cache;
    int idx = entry->inode;  /* cursor */
    if (idx < 0 || idx >= dc->count) return -1;
    *entry = dc->entries[idx];
    entry->inode = idx + 1;  /* next call starts here */
    return 0;
}

int fat32_shim_stat(const char* path, struct stat* st) {
    if (!path || !st || !fat32_is_mounted()) return -1;

    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        memset(st, 0, sizeof(*st));
        st->mode = S_IFDIR | 0755;
        st->size = 0;
        return 0;
    }

    char name83[13];
    uint32_t parent_cluster;
    if (resolve_parent_cluster(path, &parent_cluster, name83) < 0) return -1;

    struct fat32_dirent de;
    if (fat32_lookup_in_dir(parent_cluster, name83, &de) < 0) return -1;

    memset(st, 0, sizeof(*st));
    if (de.is_dir) {
        st->mode = S_IFDIR | 0755;
        st->size = 0;
    } else {
        st->mode = S_IFREG | 0644;
        st->size = de.file_size;
    }
    return 0;
}

int fat32_shim_file_size(int fd) {
    if (fd < 0 || fd >= MAX_FAT32_FDS || !fat32_fds[fd].used) return -1;
    if (fat32_fds[fd].is_dir) return -1;
    return fat32_fds[fd].size;
}

int fat32_shim_lseek(int fd, off_t offset, int whence) {
    if (fd < 0 || fd >= MAX_FAT32_FDS || !fat32_fds[fd].used) return -1;
    if (fat32_fds[fd].is_dir) return -1;
    off_t base;
    switch (whence) {
        case 0: base = 0; break;
        case 1: base = (off_t)fat32_fds[fd].offset; break;
        case 2: base = (off_t)fat32_fds[fd].size; break;
        default: return -1;
    }
    off_t new_off = base + offset;
    if (new_off < 0) return -1;
    fat32_fds[fd].offset = (int)new_off;
    return (int)new_off;
}

int fat32_shim_read_at(int fd, void* buf, int count, off_t offset) {
    if (fd < 0 || fd >= MAX_FAT32_FDS || !fat32_fds[fd].used) return -1;
    if (fat32_fds[fd].is_dir) return -1;
    if (!buf || count <= 0) return 0;
    if (offset < 0) return -1;
    if ((size_t)offset >= (size_t)fat32_fds[fd].size) return 0;
    int avail = fat32_fds[fd].size - (int)offset;
    if (count > avail) count = avail;
    memcpy(buf, fat32_fds[fd].data + offset, count);
    return count;
}
