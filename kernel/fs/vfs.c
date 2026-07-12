/*
 * Lestra OS - Virtual File System (in-memory)
 * Copyright (c) 2026 lestramk.org
 */
#include <lestra/types.h>
#include <lestra/vfs.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <string.h>

#define MAX_FILES 64
#define MAX_FILE_SIZE 65536

/* ---- ext2 plumbing (forward decls; definitions in ext2_shim.c) ----
 * The ext2 driver is a real read+write implementation but historically
 * was not reachable through VFS — only via direct ext2_read_file()
 * calls. The shim in ext2_shim.c bridges the path-based ext2 API to
 * an fd-based API so vfs_open/vfs_read/vfs_readdir can fall back to
 * ext2 when the in-memory memfs doesn't have the file. */
extern int  ext2_is_mounted(void);
extern int  ext2_open_file(const char* path);
extern int  ext2_read_fd(int fd, void* buf, int count);
extern int  ext2_close_file(int fd);
extern int  ext2_readdir(int fd, struct dirent* entry);
extern int  ext2_stat_file(const char* path, struct stat* st);

struct mem_file {
    char name[MAX_NAME_LEN];
    uint8_t data[MAX_FILE_SIZE];
    size_t size;
    int exists;
    uint32_t mode;
};

/* Per-file read offset. The in-memory VFS previously had no offset
 * tracking, so vfs_read always read from byte 0 and returned the
 * full file every call. That broke elf_exec's read loop, which
 * accumulated duplicate copies until the buffer was full. Reset
 * to 0 in vfs_open. */
static size_t fs_offsets[MAX_FILES];

static struct mem_file fs_files[MAX_FILES];
static int fs_num_files = 0;

static int find_file(const char* path) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (fs_files[i].exists && strcmp(fs_files[i].name, path) == 0) {
            return i;
        }
    }
    return -1;
}

void vfs_init(void) {
    memset(fs_files, 0, sizeof(fs_files));
    fs_num_files = 0;
    pr_info("VFS: initialized (in-memory, max %d files)\n", MAX_FILES);
}

int vfs_mount(const char* source, const char* target, const char* fs_type) {
    (void)source; (void)target; (void)fs_type;
    return 0; /* Always succeeds for in-memory FS */
}

int vfs_unmount(const char* path) {
    (void)path;
    return 0;
}

struct vnode* vfs_lookup(const char* path) {
    (void)path;
    return NULL;
}

int vfs_open(const char* path, int flags) {
    /* FIX: fds 0,1,2 are reserved for stdin/stdout/stderr.
     * Real file descriptors start at 3. */
    int existing = find_file(path);
    if (existing >= 0) return existing + 3;

    if (flags & O_CREAT) {
        for (int i = 0; i < MAX_FILES; i++) {
            if (!fs_files[i].exists) {
                strncpy(fs_files[i].name, path, MAX_NAME_LEN - 1);
                fs_files[i].name[MAX_NAME_LEN - 1] = '\0';
                fs_files[i].size = 0;
                fs_files[i].exists = 1;
                fs_files[i].mode = 0644;
                fs_num_files++;
                return i + 3;
            }
        }
    }

    /* Not in memfs. If ext2 is mounted, try to open the file there.
     * ext2 fds live in a separate fd-space above the memfs range
     * (offset 100) so they don't collide with memfs fds (3..66). */
    if (ext2_is_mounted()) {
        int efd = ext2_open_file(path);
        if (efd >= 0) {
            return efd + 100;   /* ext2 fd space */
        }
    }
    return -1;
}

/* Tag for vfs_read/write/close/readdir to know which fs an fd belongs to. */
#define VFS_FD_IS_EXT2(fd)  ((fd) >= 100)

int vfs_close(int fd) {
    if (VFS_FD_IS_EXT2(fd)) {
        return ext2_close_file(fd - 100);
    }
    int idx = fd - 3;
    if (idx >= 0 && idx < MAX_FILES && fs_files[idx].exists) {
        return 0;
    }
    return -1;
}

ssize_t vfs_read(int fd, void* buf, size_t count) {
    if (VFS_FD_IS_EXT2(fd)) {
        return (ssize_t)ext2_read_fd(fd - 100, buf, (int)count);
    }
    int idx = fd - 3;
    if (idx < 0 || idx >= MAX_FILES || !fs_files[idx].exists) return -1;
    struct mem_file* file = &fs_files[idx];
    size_t off = fs_offsets[idx];
    if (off >= file->size) {
        /* EOF. */
        return 0;
    }
    size_t to_read = count;
    if (to_read > file->size - off) to_read = file->size - off;
    memcpy(buf, file->data + off, to_read);
    fs_offsets[idx] = off + to_read;
    return (ssize_t)to_read;
}

ssize_t vfs_write(int fd, const void* buf, size_t count) {
    int idx = fd - 3;
    if (idx < 0 || idx >= MAX_FILES || !fs_files[idx].exists) return -1;
    struct mem_file* file = &fs_files[idx];
    if (count > MAX_FILE_SIZE) count = MAX_FILE_SIZE;
    memcpy(file->data, buf, count);
    file->size = count;
    return (ssize_t)count;
}

int vfs_readdir(int fd, struct dirent* entry) {
    if (!entry) return -1;
    if (VFS_FD_IS_EXT2(fd)) {
        return ext2_readdir(fd - 100, entry);
    }
    (void)fd;
    /* FIX: previous version used a single static index that broke when
     * called from multiple contexts. Use a small per-call cursor encoded
     * in entry->inode (caller passes starting index there). */
    int start = entry->inode;
    if (start < 0 || start >= MAX_FILES) start = 0;
    for (int i = start; i < MAX_FILES; i++) {
        if (fs_files[i].exists) {
            entry->inode = i + 1;  /* next call starts here */
            entry->type = FT_REGULAR;
            strncpy(entry->name, fs_files[i].name, MAX_NAME_LEN - 1);
            entry->name[MAX_NAME_LEN - 1] = '\0';
            return 0;
        }
    }
    return -1; /* End of directory */
}

int vfs_mkdir(const char* path, uint32_t mode) {
    (void)path; (void)mode;
    return -1; /* Directories not supported in simple memfs */
}

int vfs_create(const char* path, uint32_t mode) {
    return vfs_open(path, O_CREAT | mode);
}

int vfs_stat(const char* path, struct stat* st) {
    if (!st) return -1;
    int fd = find_file(path);
    if (fd < 0) {
        /* Try ext2 if mounted. */
        extern int ext2_is_mounted(void);
        extern int ext2_stat_file(const char* path, struct stat* st);
        if (ext2_is_mounted()) {
            return ext2_stat_file(path, st);
        }
        return -1;
    }
    memset(st, 0, sizeof(struct stat));
    st->mode = fs_files[fd].mode;
    st->size = fs_files[fd].size;
    return 0;
}

void vfs_register_fs(struct filesystem* fs) {
    (void)fs;
}

/* initrd loading */
void initrd_load(void* data, size_t size) {
    if (!data || size < 4) {
        pr_warn("initrd: invalid data\n");
        return;
    }

    uint32_t* ptr = (uint32_t*)data;
    uint32_t num_files = *ptr++;
    uint8_t* byte_ptr = (uint8_t*)ptr;

    uint32_t loaded = 0;
    for (uint32_t i = 0; i < num_files && i < MAX_FILES; i++) {
        char name[MAX_NAME_LEN];
        memcpy(name, byte_ptr, MAX_NAME_LEN);
        name[MAX_NAME_LEN - 1] = '\0';
        byte_ptr += MAX_NAME_LEN;

        /* Normalize the path: initrd files are stored as bare basenames
         * (e.g. "init") but userspace_boot looks up "/init" (canonical
         * absolute path). Prepend '/' if missing so the VFS lookup
         * matches. */
        char stored[MAX_NAME_LEN];
        if (name[0] == '/') {
            strncpy(stored, name, MAX_NAME_LEN - 1);
        } else {
            stored[0] = '/';
            strncpy(stored + 1, name, MAX_NAME_LEN - 2);
        }
        stored[MAX_NAME_LEN - 1] = '\0';

        uint32_t file_size = *(uint32_t*)byte_ptr;
        byte_ptr += 4;

        for (int j = 0; j < MAX_FILES; j++) {
            if (!fs_files[j].exists) {
                strncpy(fs_files[j].name, stored, MAX_NAME_LEN - 1);
                fs_files[j].name[MAX_NAME_LEN - 1] = '\0';
                fs_files[j].size = file_size;
                if (file_size > MAX_FILE_SIZE) file_size = MAX_FILE_SIZE;
                memcpy(fs_files[j].data, byte_ptr, file_size);
                fs_files[j].exists = 1;
                fs_files[j].mode = 0644;
                fs_offsets[j] = 0;
                fs_num_files++;
                loaded++;
                pr_info("initrd:   %s (%u bytes)\n", stored, file_size);
                break;
            }
        }
        byte_ptr += file_size;
    }
    pr_info("initrd: loaded %u files\n", loaded);
}

struct filesystem* initrd_get_fs(void) {
    return NULL;
}

void initrd_init(void* addr, uint32_t size) {
    initrd_load(addr, size);
}
