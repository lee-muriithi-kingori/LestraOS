/*
 * Lestra OS - FAT32 fd-shim layer (VFS bridge for read/write)
 * Copyright (c) 2026 lestramk.org
 *
 * Bridges the path-based FAT32 driver to the fd-based VFS interface.
 * Follows the same pattern as ext2_shim.c but adds write support.
 *
 * FD range: 300..315 (fat32 fd = actual fd - 300)
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
    uint8_t*  data;           /* kmalloc'd file data cache */
    struct fat32_dir_cache* dir_cache;
    char      name83[13];     /* 8.3 name for write-back */
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

/* Open a file or directory on FAT32.
 * For files: reads full contents into cache.
 * For directories: lists entries into cache.
 * Returns fd index (0..MAX_FAT32_FDS-1) or -1. */
int fat32_shim_open(const char* path) {
    if (!fat32_is_mounted() || !path) return -1;

    /* Extract the 8.3 name from path (last component)
     * For now we only support root-directory files: /FILENAME.EXT */
    const char *name83 = path;
    while (*name83 == '/') name83++;
    /* Skip any directory components (only root dir supported) */
    const char *slash = strchr(name83, '/');
    if (slash) {
        /* Subdirectory path — not supported yet via this simple shim.
         * The path-based API (fat32_lookup_in_dir) handles subdirs. */
        pr_warn("fat32-shim: subdirectory paths not yet supported: '%s'\n", path);
        return -1;
    }

    int slot = fat32_alloc_fd();
    if (slot < 0) {
        pr_warn("fat32-shim: no free fds\n");
        return -1;
    }

    /* Look up the file */
    struct fat32_dirent de;
    int found = fat32_lookup(name83, &de);

    if (found < 0 && !fat32_is_writable()) {
        /* Not found and can't create */
        return -1;
    }

    struct fat32_open_file *f = &fat32_fds[slot];
    memset(f, 0, sizeof(*f));
    strncpy(f->name83, name83, 12);
    f->name83[12] = '\0';

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
    f->first_cluster = de.first_cluster;  /* store for write-back */
    return slot;
}

/* Create a new file on FAT32 and open it.
 * Returns fd index or -1. */
int fat32_shim_create(const char* path) {
    if (!fat32_is_writable() || !path) return -1;

    const char *name83 = path;
    while (*name83 == '/') name83++;
    const char *slash = strchr(name83, '/');
    if (slash) {
        pr_warn("fat32-shim: create in subdirs not yet supported: '%s'\n", path);
        return -1;
    }

    /* Create the file */
    struct fat32_dirent de;
    if (fat32_create_file(name83, &de) < 0) {
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
        int written = fat32_write_file(f->first_cluster, 0,  /* size=0: use offset+count */
                                        f->data, (uint32_t)f->size, 0,
                                        &new_cluster, &new_size);
        if (written > 0) {
            /* Update the directory entry's file size */
            fat32_update_entry_size(f->name83, new_size);
            f->first_cluster = new_cluster;
            pr_info("fat32-shim: wrote back '%s' (%d bytes, cluster %u)\n",
                    f->name83, new_size, new_cluster);
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

    const char *name83 = path;
    while (*name83 == '/') name83++;

    struct fat32_dirent de;
    if (fat32_lookup(name83, &de) < 0) return -1;

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
