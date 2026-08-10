/*
 * Lestra OS - Virtual File System (in-memory + ext2 mount delegation)
 * Copyright (c) 2026 lestramk.org
 *
 * This VFS manages two layers:
 *   1. memfs — an in-memory filesystem with real directory support.
 *      Files and directories live in a static array (fs_files[]).
 *      Each directory tracks its children via an index list.
 *      The root directory is always at fs_files[0].
 *   2. ext2  — a real disk-backed filesystem (AHCI SATA).
 *      When ext2 is mounted via vfs_mount(), a mount table entry
 *      records the mount point path. VFS operations that can't
 *      satisfy a path in memfs delegate to the ext2 shim layer
 *      if the path falls under an ext2 mount point.
 *
 * FD-space layout:
 *   0..2    reserved (stdin/stdout/stderr)
 *   3..66   memfs files  (idx + 3)
 *   100..115 ext2 files  (ext2_shim slot + 100)
 *   300..399 procfs fds (/proc synthetic files)
 *   400..499 devfs fds  (/dev character devices)
 *
 * The VFS is also the central dispatcher: when a path starts
 * with /proc or /dev, it routes to the appropriate subsystem
 * before trying memfs/ext2.
 */
#include <lestra/types.h>
#include <lestra/vfs.h>
#include <lestra/ext2.h>
#include <lestra/procfs.h>
#include <lestra/devfs.h>
#include <lestra/fat32.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <string.h>

#define MAX_FILES       64
#define MAX_FILE_SIZE   65536

/* ---- ext2 plumbing (definitions in ext2_shim.c) ---- */
extern int  ext2_is_mounted(void);
extern int  ext2_open_file(const char* path);
extern int  ext2_read_fd(int fd, void* buf, int count);
extern int  ext2_close_file(int fd);
extern int  ext2_readdir(int fd, struct dirent* entry);
extern int  ext2_stat_file(const char* path, struct stat* st);
extern int  ext2_file_size(int fd);

/* ---- FAT32 plumbing (definitions in fat32_shim.c) ---- */
extern int  fat32_shim_open(const char* path);
extern int  fat32_shim_create(const char* path);
extern int  fat32_shim_read(int fd, void* buf, int count);
extern int  fat32_shim_write(int fd, const void* buf, int count);
extern int  fat32_shim_close(int fd);
extern int  fat32_shim_readdir(int fd, struct dirent* entry);
extern int  fat32_shim_stat(const char* path, struct stat* st);
extern int  fat32_shim_file_size(int fd);


/* ========================================================================
 * memfs: in-memory filesystem with directory support
 * ======================================================================== */

struct mem_file {
    char name[MAX_NAME_LEN];       /* basename (e.g. "init", "bin") */
    uint8_t data[MAX_FILE_SIZE];   /* file content (unused for dirs) */
    size_t size;                    /* file size; 0 for empty dirs */
    int exists;
    uint32_t mode;                  /* permissions + file type mask */
    int is_dir;                     /* 1 = directory, 0 = regular file */
    int parent_idx;                 /* index of parent dir; -1 = root dir itself */
    int children[MAX_CHILDREN];    /* child indices (for directories) */
    int num_children;              /* count of children */
};

/* Per-fd offset shared between read, write, and lseek (POSIX model).
 * Indexed by memfs slot index (fd - 3). Reset to 0 in vfs_open. */
static size_t fs_offsets[MAX_FILES];

/* Per-fd open flags so vfs_write can check O_APPEND, etc. */
static int fs_flags[MAX_FILES];

static struct mem_file fs_files[MAX_FILES];
static int fs_num_files = 0;

/* Root directory is always at index 0. */
#define ROOT_IDX 0

/* ---- memfs helpers ---- */

/* Find a child named `name` in directory at `dir_idx`.
 * Returns the child's index, or -1 if not found. */
static int memfs_find_child(int dir_idx, const char* name) {
    if (dir_idx < 0 || dir_idx >= MAX_FILES || !fs_files[dir_idx].exists)
        return -1;
    struct mem_file* dir = &fs_files[dir_idx];
    for (int i = 0; i < dir->num_children; i++) {
        int cidx = dir->children[i];
        if (cidx >= 0 && cidx < MAX_FILES && fs_files[cidx].exists
            && strcmp(fs_files[cidx].name, name) == 0) {
            return cidx;
        }
    }
    return -1;
}

/* Add a child index to a directory's children list.
 * Returns 0 on success, -1 if the children list is full. */
static int memfs_add_child(int dir_idx, int child_idx) {
    if (dir_idx < 0 || dir_idx >= MAX_FILES || !fs_files[dir_idx].is_dir)
        return -1;
    struct mem_file* dir = &fs_files[dir_idx];
    if (dir->num_children >= MAX_CHILDREN)
        return -1;
    /* Avoid duplicates. */
    for (int i = 0; i < dir->num_children; i++) {
        if (dir->children[i] == child_idx)
            return 0;  /* already a child */
    }
    dir->children[dir->num_children++] = child_idx;
    return 0;
}

/* Remove a child index from a directory's children list. */
static void memfs_remove_child(int dir_idx, int child_idx) {
    if (dir_idx < 0 || dir_idx >= MAX_FILES || !fs_files[dir_idx].is_dir)
        return;
    struct mem_file* dir = &fs_files[dir_idx];
    for (int i = 0; i < dir->num_children; i++) {
        if (dir->children[i] == child_idx) {
            /* Shift remaining children down. */
            for (int j = i; j < dir->num_children - 1; j++)
                dir->children[j] = dir->children[j + 1];
            dir->num_children--;
            return;
        }
    }
}

/* Walk an absolute path through the memfs directory tree.
 * Returns the index of the final entry, or -1 if any component
 * is not found. */
static int memfs_resolve_path(const char* path) {
    if (!path || path[0] != '/')
        return -1;

    /* Root directory. */
    if (path[1] == '\0')
        return ROOT_IDX;

    int cur = ROOT_IDX;
    const char* p = path + 1;  /* skip leading '/' */

    while (*p) {
        /* Extract the next path component. */
        char comp[MAX_NAME_LEN];
        int len = 0;
        while (*p && *p != '/' && len < MAX_NAME_LEN - 1) {
            comp[len++] = *p++;
        }
        comp[len] = '\0';
        if (*p == '/') p++;

        if (len == 0) continue;  /* skip empty components (e.g. "//") */

        /* Look up the component in the current directory's children. */
        int next = memfs_find_child(cur, comp);
        if (next < 0)
            return -1;  /* component not found */
        cur = next;
    }

    return cur;
}

/* Find the parent directory index for a given path, and extract
 * the basename. Returns parent index via *parent_out and copies
 * basename to basename_out. Returns 0 on success, -1 on error. */
static int memfs_split_path(const char* path, int* parent_out,
                             char* basename_out, size_t basename_sz) {
    if (!path || path[0] != '/')
        return -1;

    /* Special case: path is "/" — no parent/basename split. */
    if (path[1] == '\0')
        return -1;

    /* Find the last '/' in the path. */
    const char* last_slash = path;
    const char* end = path;
    while (*end) {
        if (*end == '/') last_slash = end;
        end++;
    }

    /* Copy basename (after the last slash). */
    const char* bstart = last_slash + 1;
    int bname_len = (int)(end - bstart);
    if (bname_len <= 0) return -1;
    if (bname_len >= (int)basename_sz) bname_len = (int)basename_sz - 1;
    memcpy(basename_out, bstart, bname_len);
    basename_out[bname_len] = '\0';

    /* Resolve the parent path (everything up to and including last slash,
     * converted to a directory path). */
    char parent_path[MAX_PATH_LEN];
    int parent_len = (int)(last_slash - path);
    if (parent_len == 0) parent_len = 1;  /* just "/" */
    if (parent_len >= MAX_PATH_LEN) parent_len = MAX_PATH_LEN - 1;
    memcpy(parent_path, path, parent_len);
    parent_path[parent_len] = '\0';

    /* If parent is just "/", use root. Otherwise resolve. */
    if (strcmp(parent_path, "/") == 0) {
        *parent_out = ROOT_IDX;
    } else {
        /* Ensure parent_path ends with "/" stripped for resolution
         * (e.g. "/bin" not "/bin/"). Our resolve handles both. */
        int resolved = memfs_resolve_path(parent_path);
        if (resolved < 0)
            return -1;
        *parent_out = resolved;
    }

    return 0;
}

/* Allocate a free memfs slot. Returns index, or -1 if full. */
static int memfs_alloc_slot(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (!fs_files[i].exists)
            return i;
    }
    return -1;
}

/* ========================================================================
 * Mount table
 * ======================================================================== */

static struct mount mounts[MAX_MOUNTS];
static int num_mounts = 0;

/* Check if a path falls under a given mount point.
 * For example, path="/etc/passwd" falls under mount_path="/".
 * path="/mnt/disk/foo" falls under mount_path="/mnt/disk". */
static int path_under_mount(const char* path, const char* mount_path) {
    if (!path || !mount_path) return 0;

    /* Mount at "/" covers everything. */
    if (strcmp(mount_path, "/") == 0)
        return 1;

    size_t mp_len = strlen(mount_path);
    /* Path must start with mount_path. */
    if (strncmp(path, mount_path, mp_len) != 0)
        return 0;

    /* The character after mount_path must be '/' or '\0'. */
    if (path[mp_len] == '/' || path[mp_len] == '\0')
        return 1;

    return 0;
}

/* Find the ext2 mount that covers the given path.
 * Returns mount index, or -1 if no ext2 mount covers the path. */
static int find_ext2_mount_for_path(const char* path) {
    for (int i = 0; i < num_mounts; i++) {
        if (mounts[i].fs_type == FS_TYPE_EXT2 &&
            path_under_mount(path, mounts[i].path)) {
            return i;
        }
    }
    return -1;
}

/* Find the FAT32 mount that covers the given path.
 * Returns mount index, or -1 if no FAT32 mount covers the path. */
static int find_fat32_mount_for_path(const char* path) {
    for (int i = 0; i < num_mounts; i++) {
        if (mounts[i].fs_type == FS_TYPE_FAT32 &&
            path_under_mount(path, mounts[i].path)) {
            return i;
        }
    }
    return -1;
}

/* ========================================================================
 * VFS public API
 * ======================================================================== */

void vfs_init(void) {
    memset(fs_files, 0, sizeof(fs_files));
    memset(fs_offsets, 0, sizeof(fs_offsets));
    memset(fs_flags, 0, sizeof(fs_flags));
    memset(mounts, 0, sizeof(mounts));
    fs_num_files = 0;
    num_mounts = 0;

    /* Create the root directory at index 0. */
    fs_files[ROOT_IDX].exists   = 1;
    fs_files[ROOT_IDX].is_dir   = 1;
    fs_files[ROOT_IDX].mode     = S_IFDIR | 0755;
    fs_files[ROOT_IDX].size     = 0;
    fs_files[ROOT_IDX].parent_idx = -1;   /* root has no parent */
    fs_files[ROOT_IDX].num_children = 0;
    strncpy(fs_files[ROOT_IDX].name, "/", MAX_NAME_LEN - 1);
    fs_num_files = 1;

    pr_info("VFS: initialized (memfs, max %d files, root dir at idx %d)\n",
            MAX_FILES, ROOT_IDX);
}

int vfs_mount(const char* source, const char* target, const char* fs_type) {
    if (!target || !fs_type) return -1;

    /* Check for duplicate mount at the same target. */
    for (int i = 0; i < num_mounts; i++) {
        if (strcmp(mounts[i].path, target) == 0) {
            pr_warn("VFS: mount point '%s' already in use\n", target);
            return -1;
        }
    }

    if (num_mounts >= MAX_MOUNTS) {
        pr_warn("VFS: mount table full (%d mounts)\n", MAX_MOUNTS);
        return -1;
    }

    if (strcmp(fs_type, "ext2") == 0) {
        /* Mount the ext2 filesystem. ext2_mount() initializes the
         * driver (reads superblock, etc.) and sets ext2_mounted=1. */
        if (!ext2_is_mounted()) {
            if (!ext2_mount()) {
                pr_warn("VFS: ext2 mount failed\n");
                return -1;
            }
        }

        /* Record the mount in the table. */
        int mi = num_mounts;
        strncpy(mounts[mi].path, target, MAX_PATH_LEN - 1);
        mounts[mi].path[MAX_PATH_LEN - 1] = '\0';
        mounts[mi].fs_type = FS_TYPE_EXT2;
        mounts[mi].root = NULL;     /* ext2 doesn't use vnodes */
        mounts[mi].fs = NULL;
        mounts[mi].private_data = NULL;

        /* If the mount point isn't "/" and doesn't exist in memfs,
         * create a stub directory so memfs path resolution can
         * "cross" into ext2 at this point. */
        if (strcmp(target, "/") != 0) {
            int existing = memfs_resolve_path(target);
            if (existing < 0) {
                /* Create stub directory for the mount point. */
                char basename_buf[MAX_NAME_LEN];
                int parent_idx;
                if (memfs_split_path(target, &parent_idx,
                                     basename_buf, sizeof(basename_buf)) == 0) {
                    int slot = memfs_alloc_slot();
                    if (slot >= 0) {
                        strncpy(fs_files[slot].name, basename_buf, MAX_NAME_LEN - 1);
                        fs_files[slot].name[MAX_NAME_LEN - 1] = '\0';
                        fs_files[slot].exists   = 1;
                        fs_files[slot].is_dir   = 1;
                        fs_files[slot].mode     = S_IFDIR | 0755;
                        fs_files[slot].size     = 0;
                        fs_files[slot].parent_idx = parent_idx;
                        fs_files[slot].num_children = 0;
                        memfs_add_child(parent_idx, slot);
                        fs_num_files++;
                    }
                }
            }
        }

        num_mounts++;
        pr_info("VFS: ext2 mounted at '%s' (mount #%d)\n", target, mi);
        return 0;
    }

    /* "memfs" or unknown type — for memfs we just record it
     * (initrd is already loaded). Silently succeed for memfs. */
    if (strcmp(fs_type, "memfs") == 0) {
        int mi = num_mounts;
        strncpy(mounts[mi].path, target, MAX_PATH_LEN - 1);
        mounts[mi].path[MAX_PATH_LEN - 1] = '\0';
        mounts[mi].fs_type = FS_TYPE_MEMFS;
        num_mounts++;
        pr_info("VFS: memfs registered at '%s'\n", target);
        return 0;
    }

    if (strcmp(fs_type, "fat32") == 0) {
        if (!fat32_is_mounted()) {
            pr_warn("VFS: FAT32 not mounted (block driver not initialized?)\n");
            return -1;
        }
        int mi = num_mounts;
        strncpy(mounts[mi].path, target, MAX_PATH_LEN - 1);
        mounts[mi].path[MAX_PATH_LEN - 1] = '\0';
        mounts[mi].fs_type = FS_TYPE_FAT32;
        mounts[mi].root = NULL;
        mounts[mi].fs = NULL;
        mounts[mi].private_data = NULL;

        /* Create a stub directory in memfs for the mount point. */
        if (strcmp(target, "/") != 0) {
            int existing = memfs_resolve_path(target);
            if (existing < 0) {
                char basename_buf[MAX_NAME_LEN];
                int parent_idx;
                if (memfs_split_path(target, &parent_idx,
                                     basename_buf, sizeof(basename_buf)) == 0) {
                    int slot = memfs_alloc_slot();
                    if (slot >= 0) {
                        strncpy(fs_files[slot].name, basename_buf, MAX_NAME_LEN - 1);
                        fs_files[slot].name[MAX_NAME_LEN - 1] = '\0';
                        fs_files[slot].exists   = 1;
                        fs_files[slot].is_dir   = 1;
                        fs_files[slot].mode     = S_IFDIR | 0755;
                        fs_files[slot].size     = 0;
                        fs_files[slot].parent_idx = parent_idx;
                        fs_files[slot].num_children = 0;
                        memfs_add_child(parent_idx, slot);
                        fs_num_files++;
                    }
                }
            }
        }

        num_mounts++;
        pr_info("VFS: FAT32 mounted at '%s' (mount #%d, %s)\n",
                target, mi, fat32_is_writable() ? "read-write" : "read-only");
        return 0;
    }

    pr_warn("VFS: unknown fs_type '%s'\n", fs_type);
    return -1;
}

int vfs_unmount(const char* path) {
    if (!path) return -1;
    for (int i = 0; i < num_mounts; i++) {
        if (strcmp(mounts[i].path, path) == 0) {
            /* Remove the mount entry by shifting the rest down. */
            for (int j = i; j < num_mounts - 1; j++)
                mounts[j] = mounts[j + 1];
            num_mounts--;
            pr_info("VFS: unmounted '%s'\n", path);
            return 0;
        }
    }
    return -1;
}

/* vfs_resolve_path: walk from root through directories and mount
 * points to find the target. Returns the memfs file index if the
 * path is in memfs, or -1 if the path should be handled by ext2
 * (or simply not found).
 *
 * The caller can then check the mount table to decide whether to
 * delegate to ext2 for a path that wasn't found in memfs. */
int vfs_resolve_path(const char* path) {
    if (!path || path[0] != '/')
        return -1;
    return memfs_resolve_path(path);
}

struct vnode* vfs_lookup(const char* path) {
    /* We don't allocate real vnodes in the current implementation.
     * Return NULL for now — the path resolution is done by
     * vfs_resolve_path() and the callers use the index directly. */
    (void)path;
    return NULL;
}

/* FD range helpers for the dispatcher. Each subsystem owns a
 * distinct range so we can route read/write/close/lseek calls
 * without a per-process fd table. */
#define VFS_FD_IS_MEMFS(fd)   ((fd) >= 3 && (fd) < 3 + MAX_FILES)
#define VFS_FD_IS_EXT2(fd)    ((fd) >= 100 && (fd) < 100 + 16)
#define VFS_FD_IS_FAT32(fd)   ((fd) >= 200 && (fd) < 200 + 16)
#define VFS_FD_IS_PROCFS(fd)  ((fd) >= PROCFS_FD_BASE && (fd) < PROCFS_FD_BASE + PROCFS_MAX_OPEN)
#define VFS_FD_IS_DEVFS(fd)   ((fd) >= DEVFS_FD_BASE && (fd) < DEVFS_FD_BASE + DEVFS_MAX_OPEN)

int vfs_open(const char* path, int flags) {
    if (!path) return -1;

    /* 0. Route /proc and /dev paths to their subsystems first. */
    if (path[0] == '/' && strncmp(path, "/proc", 5) == 0) {
        int pfd = procfs_open(path);
        if (pfd >= 0) return pfd;
        /* If procfs doesn't handle it, fall through to memfs/ext2. */
    }
    if (path[0] == '/' && strncmp(path, "/dev", 4) == 0) {
        int dfd = devfs_open(path);
        if (dfd >= 0) return dfd;
    }

    /* 1. Try memfs first. */
    int idx = memfs_resolve_path(path);

    if (idx >= 0) {
        /* Found in memfs. If O_CREAT on an existing file, just open it.
         * If O_TRUNC, reset the file size. */
        if ((flags & O_TRUNC) && !fs_files[idx].is_dir) {
            fs_files[idx].size = 0;
        }
        /* Reset offset and record flags (POSIX: open resets file offset). */
        fs_offsets[idx] = 0;
        fs_flags[idx] = flags;
        return idx + 3;  /* memfs fd space: 3..66 */
    }

    /* 2. O_CREAT: create a new file in memfs (or on ext2). */
    if (flags & O_CREAT) {
        /* Split the path into parent + basename. */
        char basename_buf[MAX_NAME_LEN];
        int parent_idx;
        if (memfs_split_path(path, &parent_idx,
                             basename_buf, sizeof(basename_buf)) < 0) {
            /* Can't determine parent — maybe parent doesn't exist.
             * Check if the path falls under an ext2 mount; if so,
             * delegate creation to ext2. */
            int emi = find_ext2_mount_for_path(path);
            if (emi >= 0) {
                /* ext2 supports file creation. */
                uint32_t ino = ext2_create_file(path, 0);
                if (ino > 0) {
                    /* Open the newly created ext2 file via the shim. */
                    int efd = ext2_open_file(path);
                    if (efd >= 0)
                        return efd + 100;
                }
            }
            return -1;
        }

        /* Parent exists in memfs — create the file there. */
        int slot = memfs_alloc_slot();
        if (slot < 0) {
            pr_warn("VFS: memfs full, cannot create '%s'\n", path);
            return -1;
        }

        strncpy(fs_files[slot].name, basename_buf, MAX_NAME_LEN - 1);
        fs_files[slot].name[MAX_NAME_LEN - 1] = '\0';
        fs_files[slot].exists   = 1;
        fs_files[slot].is_dir   = 0;
        fs_files[slot].mode     = S_IFREG | 0644;
        fs_files[slot].size     = 0;
        fs_files[slot].parent_idx = parent_idx;
        fs_files[slot].num_children = 0;
        fs_offsets[slot] = 0;
        fs_flags[slot] = flags;   /* remember O_APPEND etc. */
        memfs_add_child(parent_idx, slot);
        fs_num_files++;

        pr_info("VFS: created '%s' (memfs idx %d)\n", path, slot);
        return slot + 3;
    }

    /* 3. Not in memfs, no O_CREAT. Try ext2 if path falls under
     *    an ext2 mount point. */
    int emi = find_ext2_mount_for_path(path);
    if (emi >= 0) {
        int efd = ext2_open_file(path);
        if (efd >= 0) {
            return efd + 100;   /* ext2 fd space */
        }
    }

    /* 4. Try FAT32 if path falls under a FAT32 mount point. */
    int fmi = find_fat32_mount_for_path(path);
    if (fmi >= 0) {
        int ffd = fat32_shim_open(path);
        if (ffd >= 0) {
            return ffd + 200;   /* FAT32 fd space */
        }
    }

    return -1;
}

int vfs_close(int fd) {
    if (VFS_FD_IS_PROCFS(fd)) {
        return procfs_close(fd);
    }
    if (VFS_FD_IS_DEVFS(fd)) {
        return devfs_close(fd);
    }
    if (VFS_FD_IS_EXT2(fd)) {
        return ext2_close_file(fd - 100);
    }
    if (VFS_FD_IS_FAT32(fd)) {
        return fat32_shim_close(fd - 200);
    }
    int idx = fd - 3;
    if (idx >= 0 && idx < MAX_FILES && fs_files[idx].exists) {
        return 0;
    }
    return -1;
}

ssize_t vfs_read(int fd, void* buf, size_t count) {
    if (VFS_FD_IS_PROCFS(fd)) {
        return procfs_read(fd, buf, count);
    }
    if (VFS_FD_IS_DEVFS(fd)) {
        return devfs_read(fd, buf, count);
    }
    if (VFS_FD_IS_EXT2(fd)) {
        return (ssize_t)ext2_read_fd(fd - 100, buf, (int)count);
    }
    if (VFS_FD_IS_FAT32(fd)) {
        return (ssize_t)fat32_shim_read(fd - 200, buf, (int)count);
    }
    int idx = fd - 3;
    if (idx < 0 || idx >= MAX_FILES || !fs_files[idx].exists) return -1;

    struct mem_file* file = &fs_files[idx];

    /* Can't read a directory like a regular file. */
    if (file->is_dir) return -1;

    size_t off = fs_offsets[idx];
    if (off >= file->size) {
        return 0;  /* EOF */
    }
    size_t to_read = count;
    if (to_read > file->size - off) to_read = file->size - off;
    memcpy(buf, file->data + off, to_read);
    fs_offsets[idx] = off + to_read;
    return (ssize_t)to_read;
}

ssize_t vfs_write(int fd, const void* buf, size_t count) {
    if (VFS_FD_IS_DEVFS(fd)) {
        return devfs_write(fd, buf, count);
    }
    if (VFS_FD_IS_PROCFS(fd)) {
        /* /proc files are read-only synthetic files. */
        return -1;
    }
    if (VFS_FD_IS_EXT2(fd)) {
        /* ext2 write support is not fd-based in the current shim.
         * The path-based ext2_write_file() exists but we don't
         * track the path per-fd here. For now, return -1.
         * Future: add write support to ext2_shim. */
        return -1;
    }
    if (VFS_FD_IS_FAT32(fd)) {
        return (ssize_t)fat32_shim_write(fd - 200, buf, (int)count);
    }
    int idx = fd - 3;
    if (idx < 0 || idx >= MAX_FILES || !fs_files[idx].exists) return -1;

    struct mem_file* file = &fs_files[idx];
    if (file->is_dir) return -1;  /* can't write to a directory */

    if (count > MAX_FILE_SIZE) count = MAX_FILE_SIZE;

    /* Offset-aware write: use fs_offsets[idx] as the starting
     * position (shared with read/lseek per POSIX). If O_APPEND
     * was set on open, seek to end before writing. */
    size_t off = fs_offsets[idx];
    if (fs_flags[idx] & O_APPEND) {
        off = file->size;
    }

    /* Calculate how many bytes we can write starting at offset.
     * We allow the file to grow but cap at MAX_FILE_SIZE. */
    size_t end = off + count;
    if (end > MAX_FILE_SIZE) {
        count = MAX_FILE_SIZE - off;
        if (count == 0) return 0;  /* file is full */
    }

    /* Write data at the current offset (partial write — only
     * overwrites the region [off, off+count), preserving any
     * data before or after this region). */
    memcpy(file->data + off, buf, count);

    /* Update file size if we extended past the old end. */
    if (end > file->size) {
        file->size = end;
    }

    /* Advance the shared offset (POSIX: write moves the cursor). */
    fs_offsets[idx] = off + count;
    return (ssize_t)count;
}

/* vfs_readdir: list the children of a directory.
 *
 * For memfs directory fds: iterate through the children of
 * the mem_file at (fd-3), using entry->inode as a cursor
 * (index into the children[] array).
 *
 * For ext2 fds: delegate to ext2_readdir in the shim.
 *
 * For the legacy fd=0 (stdin) or any non-directory memfs fd:
 * fall back to listing all root-level files (the old behavior)
 * for backward compatibility with callers that don't open a
 * directory first. */
int vfs_readdir(int fd, struct dirent* entry) {
    if (!entry) return -1;

    /* ext2 fd delegation. */
    if (VFS_FD_IS_EXT2(fd)) {
        return ext2_readdir(fd - 100, entry);
    }

    /* FAT32 fd delegation. */
    if (VFS_FD_IS_FAT32(fd)) {
        return fat32_shim_readdir(fd - 200, entry);
    }

    int idx = fd - 3;

    /* If idx is a valid memfs directory, list its children. */
    if (idx >= 0 && idx < MAX_FILES && fs_files[idx].exists
        && fs_files[idx].is_dir) {
        struct mem_file* dir = &fs_files[idx];
        int child_cursor = entry->inode;  /* cursor: which child index to start at */
        if (child_cursor < 0 || child_cursor >= dir->num_children)
            return -1;  /* end of directory */

        int cidx = dir->children[child_cursor];
        if (cidx < 0 || cidx >= MAX_FILES || !fs_files[cidx].exists)
            return -1;

        entry->inode = child_cursor + 1;  /* next call starts at next child */
        entry->type = fs_files[cidx].is_dir ? FT_DIRECTORY : FT_REGULAR;
        strncpy(entry->name, fs_files[cidx].name, MAX_NAME_LEN - 1);
        entry->name[MAX_NAME_LEN - 1] = '\0';
        entry->reclen = sizeof(struct dirent);
        return 0;
    }

    /* Legacy fallback: fd=0 or non-directory fd → list root children.
     * This preserves backward compatibility with code that calls
     * vfs_readdir(0, &entry) to list all files. */
    struct mem_file* root = &fs_files[ROOT_IDX];
    int child_cursor = entry->inode;
    if (child_cursor < 0 || child_cursor >= root->num_children)
        return -1;

    int cidx = root->children[child_cursor];
    if (cidx < 0 || cidx >= MAX_FILES || !fs_files[cidx].exists)
        return -1;

    entry->inode = child_cursor + 1;
    entry->type = fs_files[cidx].is_dir ? FT_DIRECTORY : FT_REGULAR;
    strncpy(entry->name, fs_files[cidx].name, MAX_NAME_LEN - 1);
    entry->name[MAX_NAME_LEN - 1] = '\0';
    entry->reclen = sizeof(struct dirent);
    return 0;
}

int vfs_mkdir(const char* path, uint32_t mode) {
    if (!path || path[0] != '/') return -1;

    /* Check if the path already exists in memfs. */
    int existing = memfs_resolve_path(path);
    if (existing >= 0) {
        /* Already exists — return error (EEXIST semantics). */
        return -1;
    }

    /* Split the path into parent + basename. */
    char basename_buf[MAX_NAME_LEN];
    int parent_idx;
    if (memfs_split_path(path, &parent_idx,
                         basename_buf, sizeof(basename_buf)) < 0) {
        /* Can't determine parent. If parent doesn't exist in memfs
         * but falls under an ext2 mount, delegate to ext2_mkdir. */
        int emi = find_ext2_mount_for_path(path);
        if (emi >= 0 && ext2_is_mounted()) {
            uint32_t ino = ext2_mkdir(path, (uint16_t)(mode & 0777));
            return (ino > 0) ? 0 : -1;
        }
        return -1;
    }

    /* Parent must be a directory in memfs. */
    if (!fs_files[parent_idx].is_dir) return -1;

    /* Allocate a slot for the new directory. */
    int slot = memfs_alloc_slot();
    if (slot < 0) {
        pr_warn("VFS: memfs full, cannot mkdir '%s'\n", path);
        /* Try ext2 as fallback. */
        int emi = find_ext2_mount_for_path(path);
        if (emi >= 0 && ext2_is_mounted()) {
            uint32_t ino = ext2_mkdir(path, (uint16_t)(mode & 0777));
            return (ino > 0) ? 0 : -1;
        }
        return -1;
    }

    uint32_t dir_mode = S_IFDIR | (mode ? (mode & 0777) : 0755);

    strncpy(fs_files[slot].name, basename_buf, MAX_NAME_LEN - 1);
    fs_files[slot].name[MAX_NAME_LEN - 1] = '\0';
    fs_files[slot].exists     = 1;
    fs_files[slot].is_dir     = 1;
    fs_files[slot].mode       = dir_mode;
    fs_files[slot].size       = 0;
    fs_files[slot].parent_idx = parent_idx;
    fs_files[slot].num_children = 0;

    memfs_add_child(parent_idx, slot);
    fs_num_files++;

    pr_info("VFS: mkdir '%s' (memfs idx %d, mode 0%o)\n", path, slot, dir_mode);
    return 0;
}

int vfs_create(const char* path, uint32_t mode) {
    return vfs_open(path, O_CREAT | (int)(mode & 0777));
}

int vfs_stat(const char* path, struct stat* st) {
    if (!st) return -1;

    /* Try memfs first. */
    int idx = memfs_resolve_path(path);
    if (idx >= 0) {
        memset(st, 0, sizeof(struct stat));
        st->mode = fs_files[idx].mode;
        st->size = fs_files[idx].size;
        /* Directories report a "size" of 0 (or num_children * entry_size
         * for a more accurate value). */
        if (fs_files[idx].is_dir)
            st->size = 0;
        return 0;
    }

    /* Not in memfs — try ext2 if path is under an ext2 mount. */
    int emi = find_ext2_mount_for_path(path);
    if (emi >= 0 && ext2_is_mounted()) {
        return ext2_stat_file(path, st);
    }

    return -1;
}

int vfs_unlink(const char* path) {
    if (!path || path[0] != '/') return -1;

    /* Try memfs first. */
    int idx = memfs_resolve_path(path);
    if (idx >= 0) {
        /* Can't unlink the root directory. */
        if (idx == ROOT_IDX) return -1;
        /* Can't unlink a non-empty directory. */
        if (fs_files[idx].is_dir && fs_files[idx].num_children > 0)
            return -1;

        int parent = fs_files[idx].parent_idx;
        if (parent >= 0 && parent < MAX_FILES) {
            memfs_remove_child(parent, idx);
        }

        fs_files[idx].exists = 0;
        fs_files[idx].num_children = 0;
        fs_num_files--;
        pr_info("VFS: unlink '%s'\n", path);
        return 0;
    }

    /* Try ext2. */
    int emi = find_ext2_mount_for_path(path);
    if (emi >= 0 && ext2_is_mounted()) {
        return ext2_unlink(path) > 0 ? 0 : -1;
    }

    return -1;
}

/* vfs_chmod — change permission bits on a memfs file or directory.
 * Preserves the file-type bits (S_IFMT) and replaces only the permission
 * bits (mode & 0777). Returns 0 on success, -1 if the path doesn't exist
 * in memfs (ext2 chmod not yet supported — falls through to -1). */
int vfs_chmod(const char* path, uint32_t mode) {
    if (!path || path[0] != '/') return -1;

    int idx = memfs_resolve_path(path);
    if (idx < 0) {
        /* Not in memfs — ext2 chmod not implemented yet. */
        return -1;
    }

    /* Preserve file-type bits, set only the permission bits. */
    uint32_t type_bits = fs_files[idx].mode & S_IFMT;
    fs_files[idx].mode = type_bits | (mode & 0777);
    pr_info("VFS: chmod '%s' mode 0%o\n", path, fs_files[idx].mode);
    return 0;
}

/* vfs_rmdir — remove an EMPTY directory from memfs.
 * Returns 0 on success. Returns -1 if:
 *   - path is NULL or doesn't start with '/'
 *   - the path doesn't exist in memfs
 *   - the target is not a directory
 *   - the target is the root directory (idx 0)
 *   - the directory is not empty (num_children > 0)
 * (ext2 rmdir not yet supported — falls through to -1.) */
int vfs_rmdir(const char* path) {
    if (!path || path[0] != '/') return -1;

    int idx = memfs_resolve_path(path);
    if (idx < 0) {
        /* Not in memfs — ext2 rmdir not implemented yet. */
        return -1;
    }

    /* Can't rmdir the root. */
    if (idx == ROOT_IDX) return -1;
    /* Must be a directory. */
    if (!fs_files[idx].is_dir) return -1;
    /* Must be empty. */
    if (fs_files[idx].num_children > 0) return -1;

    /* Detach from parent's child list and free the slot. */
    int parent = fs_files[idx].parent_idx;
    if (parent >= 0 && parent < MAX_FILES) {
        memfs_remove_child(parent, idx);
    }

    fs_files[idx].exists = 0;
    fs_files[idx].num_children = 0;
    fs_num_files--;
    pr_info("VFS: rmdir '%s'\n", path);
    return 0;
}

/* vfs_selftest — exercise mkdir, create, stat, chmod, rmdir, and the
 * error paths (rmdir non-empty, rmdir non-dir, chmod nonexistent).
 * Prints a PASS/FAIL line for each check and a summary at the end.
 * Called from kernel_main after initrd load. */
void vfs_selftest(void) {
    int pass = 0, fail = 0;

    pr_info("vfs_selftest: starting (8 checks)\n");

    /* 1. mkdir /tmp — should succeed. */
    if (vfs_mkdir("/tmp", 0755) == 0) { pass++; pr_info("vfs_selftest: 1/8 PASS mkdir /tmp\n"); }
    else { fail++; pr_warn("vfs_selftest: 1/8 FAIL mkdir /tmp\n"); }

    /* 2. mkdir /tmp (again) — should fail (EEXIST). */
    if (vfs_mkdir("/tmp", 0755) < 0) { pass++; pr_info("vfs_selftest: 2/8 PASS mkdir /tmp (EEXIST)\n"); }
    else { fail++; pr_warn("vfs_selftest: 2/8 FAIL mkdir /tmp (should have failed)\n"); }

    /* 3. create /tmp/hello.txt — should succeed. */
    int fd = vfs_open("/tmp/hello.txt", O_CREAT);
    if (fd >= 0) { pass++; pr_info("vfs_selftest: 3/8 PASS create /tmp/hello.txt\n"); vfs_close(fd); }
    else { fail++; pr_warn("vfs_selftest: 3/8 FAIL create /tmp/hello.txt\n"); }

    /* 4. stat /tmp/hello.txt — should be a regular file. */
    struct stat st;
    if (vfs_stat("/tmp/hello.txt", &st) == 0 && S_ISREG(st.mode)) { pass++; pr_info("vfs_selftest: 4/8 PASS stat /tmp/hello.txt (regular)\n"); }
    else { fail++; pr_warn("vfs_selftest: 4/8 FAIL stat /tmp/hello.txt\n"); }

    /* 5. chmod /tmp/hello.txt 0644 — should succeed; verify mode bits. */
    if (vfs_chmod("/tmp/hello.txt", 0644) == 0) {
        struct stat st2;
        if (vfs_stat("/tmp/hello.txt", &st2) == 0 && (st2.mode & 0777) == 0644) { pass++; pr_info("vfs_selftest: 5/8 PASS chmod /tmp/hello.txt 0644\n"); }
        else { fail++; pr_warn("vfs_selftest: 5/8 FAIL chmod mode not applied\n"); }
    } else { fail++; pr_warn("vfs_selftest: 5/8 FAIL chmod /tmp/hello.txt\n"); }

    /* 6. rmdir /tmp (non-empty) — should fail. */
    if (vfs_rmdir("/tmp") < 0) { pass++; pr_info("vfs_selftest: 6/8 PASS rmdir /tmp (non-empty, ENOTEMPTY)\n"); }
    else { fail++; pr_warn("vfs_selftest: 6/8 FAIL rmdir /tmp (should have failed — not empty)\n"); }

    /* 7. rmdir /tmp/hello.txt (not a dir) — should fail. */
    if (vfs_rmdir("/tmp/hello.txt") < 0) { pass++; pr_info("vfs_selftest: 7/8 PASS rmdir /tmp/hello.txt (ENOTDIR)\n"); }
    else { fail++; pr_warn("vfs_selftest: 7/8 FAIL rmdir /tmp/hello.txt (should have failed — not a dir)\n"); }

    /* 8. cleanup: unlink the file, then rmdir /tmp (now empty) — should succeed. */
    vfs_unlink("/tmp/hello.txt");
    if (vfs_rmdir("/tmp") == 0) { pass++; pr_info("vfs_selftest: 8/8 PASS rmdir /tmp (empty, cleaned up)\n"); }
    else { fail++; pr_warn("vfs_selftest: 8/8 FAIL rmdir /tmp (should have succeeded)\n"); }

    pr_info("vfs_selftest: %d/%d PASS, %d FAIL\n", pass, pass + fail, fail);
}

off_t vfs_lseek(int fd, off_t offset, int whence) {
    /* SEEK_SET=0, SEEK_CUR=1, SEEK_END=2.
     * The unified fs_offsets[] array is shared between read, write,
     * and lseek so sequential I/O and seeking all advance the same
     * cursor — matching POSIX semantics. */
    if (VFS_FD_IS_PROCFS(fd)) {
        /* procfs tracks pos internally; no lseek API exposed yet.
         * Return -1 (procfs files are small; read in one shot). */
        return -1;
    }
    if (VFS_FD_IS_DEVFS(fd)) {
        /* Character devices don't support lseek. */
        return -1;
    }
    if (VFS_FD_IS_EXT2(fd)) {
        /* ext2 shim supports SEEK_END by returning cached file size.
         * For SEEK_SET/SEEK_CUR we'd need to modify the shim's
         * internal offset, which isn't exposed yet. */
        if (whence == 2) {
            int sz = ext2_file_size(fd - 100);
            if (sz < 0) return -1;
            return (off_t)(sz + offset);
        }
        /* SEEK_SET/SEEK_CUR for ext2 — shim doesn't expose offset. */
        return -1;
    }
    if (VFS_FD_IS_FAT32(fd)) {
        if (whence == 2) {
            int sz = fat32_shim_file_size(fd - 200);
            if (sz < 0) return -1;
            return (off_t)(sz + offset);
        }
        return -1;
    }
    /* memfs */
    int idx = fd - 3;
    if (idx < 0 || idx >= MAX_FILES || !fs_files[idx].exists) return -1;

    off_t base;
    switch (whence) {
        case 0: base = 0; break;                           /* SEEK_SET */
        case 1: base = (off_t)fs_offsets[idx]; break;     /* SEEK_CUR */
        case 2: base = (off_t)fs_files[idx].size; break;  /* SEEK_END — we know the memfs file size */
        default: return -1;
    }

    off_t new_off = base + offset;
    if (new_off < 0) return -1;
    /* For memfs we allow seeking past EOF (like Linux) so subsequent
     * writes can fill the gap. But reads at EOF return 0. */
    fs_offsets[idx] = (size_t)new_off;
    return new_off;
}

/* vfs_read_at / vfs_write_at — positional I/O (pread/pwrite style).
 * These read or write at a specific offset without moving the
 * shared fd offset (fs_offsets[]). For memfs files the offset
 * parameter specifies the exact byte position. For procfs/devfs
 * fds, the subsystem's internal pos tracking is used instead of
 * the offset parameter (since these synthetic files don't support
 * arbitrary positional reads). */
ssize_t vfs_read_at(int fd, void* buf, size_t count, off_t offset) {
    /* procfs: uses its own internal pos tracking, so the offset
     * parameter is effectively ignored. The syscall layer advances
     * its per-process offset based on the return value. */
    if (VFS_FD_IS_PROCFS(fd)) {
        return procfs_read(fd, buf, count);
    }
    /* devfs: reads don't have meaningful offsets (character devices). */
    if (VFS_FD_IS_DEVFS(fd)) {
        return devfs_read(fd, buf, count);
    }
    /* ext2: shim doesn't support pread-style positional reads. */
    if (VFS_FD_IS_EXT2(fd)) {
        /* Fall back to regular read (which uses ext2's internal offset). */
        return vfs_read(fd, buf, count);
    }
    /* memfs: true positional read at the given offset. */
    int idx = fd - 3;
    if (idx < 0 || idx >= MAX_FILES || !fs_files[idx].exists) return -1;
    struct mem_file* file = &fs_files[idx];
    if (file->is_dir) return -1;
    if ((size_t)offset >= file->size) return 0;
    size_t to_read = count;
    if (to_read > file->size - (size_t)offset) to_read = file->size - (size_t)offset;
    memcpy(buf, file->data + (size_t)offset, to_read);
    /* Note: does NOT advance fs_offsets[idx] (pread semantics). */
    return (ssize_t)to_read;
}

ssize_t vfs_write_at(int fd, const void* buf, size_t count, off_t offset) {
    /* procfs: synthetic files are read-only. */
    if (VFS_FD_IS_PROCFS(fd)) {
        return -1;
    }
    /* devfs: writes are always accepted (null/zero/urandom discard). */
    if (VFS_FD_IS_DEVFS(fd)) {
        return devfs_write(fd, buf, count);
    }
    /* ext2: shim doesn't support pwrite-style positional writes. */
    if (VFS_FD_IS_EXT2(fd)) {
        return -1;
    }
    /* memfs: true positional write at the given offset. */
    int idx = fd - 3;
    if (idx < 0 || idx >= MAX_FILES || !fs_files[idx].exists) return -1;
    struct mem_file* file = &fs_files[idx];
    if (file->is_dir) return -1;
    if (count > MAX_FILE_SIZE) count = MAX_FILE_SIZE;
    size_t off = (size_t)offset;
    size_t end = off + count;
    if (end > MAX_FILE_SIZE) {
        count = MAX_FILE_SIZE - off;
        if (count == 0) return 0;
    }
    memcpy(file->data + off, buf, count);
    if (end > file->size) file->size = end;
    /* Note: does NOT advance fs_offsets[idx] (pwrite semantics). */
    return (ssize_t)count;
}

void vfs_register_fs(struct filesystem* fs) {
    (void)fs;
    /* Placeholder — in a more complete VFS, this would register
     * the filesystem driver for auto-mounting. */
}

struct mount* vfs_get_mount(int idx) {
    if (idx < 0 || idx >= num_mounts) return NULL;
    return &mounts[idx];
}

int vfs_get_mount_count(void) {
    return num_mounts;
}

/* ========================================================================
 * initrd loading
 * ======================================================================== */

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
        char raw_name[MAX_NAME_LEN];
        memcpy(raw_name, byte_ptr, MAX_NAME_LEN);
        raw_name[MAX_NAME_LEN - 1] = '\0';
        byte_ptr += MAX_NAME_LEN;

        /* Normalize: prepend '/' if missing. */
        char full_path[MAX_NAME_LEN];
        if (raw_name[0] == '/') {
            strncpy(full_path, raw_name, MAX_NAME_LEN - 1);
        } else {
            full_path[0] = '/';
            strncpy(full_path + 1, raw_name, MAX_NAME_LEN - 2);
        }
        full_path[MAX_NAME_LEN - 1] = '\0';

        uint32_t file_size = *(uint32_t*)byte_ptr;
        byte_ptr += 4;

        /* Determine parent directory and basename.
         * Initrd files are typically root-level ("/init", "/shell").
         * For nested paths ("/bin/hello"), we walk/create the
         * intermediate directories. */
        char basename_buf[MAX_NAME_LEN];
        int parent_idx = ROOT_IDX;  /* default parent is root */

        /* Walk the path to find/create parent directories. */
        const char* p = full_path + 1;  /* skip leading '/' */
        int cur_dir = ROOT_IDX;

        while (*p) {
            char comp[MAX_NAME_LEN];
            int len = 0;
            while (*p && *p != '/' && len < MAX_NAME_LEN - 1) {
                comp[len++] = *p++;
            }
            comp[len] = '\0';
            if (*p == '/') {
                p++;
                /* This component is a directory in the path.
                 * Find it, or create it. */
                int child = memfs_find_child(cur_dir, comp);
                if (child < 0) {
                    /* Create intermediate directory. */
                    int slot = memfs_alloc_slot();
                    if (slot < 0) break;
                    strncpy(fs_files[slot].name, comp, MAX_NAME_LEN - 1);
                    fs_files[slot].name[MAX_NAME_LEN - 1] = '\0';
                    fs_files[slot].exists     = 1;
                    fs_files[slot].is_dir     = 1;
                    fs_files[slot].mode       = S_IFDIR | 0755;
                    fs_files[slot].size       = 0;
                    fs_files[slot].parent_idx = cur_dir;
                    fs_files[slot].num_children = 0;
                    memfs_add_child(cur_dir, slot);
                    fs_num_files++;
                    child = slot;
                }
                cur_dir = child;
            } else {
                /* This is the final component — the basename. */
                strncpy(basename_buf, comp, MAX_NAME_LEN - 1);
                basename_buf[MAX_NAME_LEN - 1] = '\0';
                parent_idx = cur_dir;
            }
        }

        /* If the path was just "/" (no components), skip. */
        if (full_path[1] == '\0') {
            byte_ptr += file_size;
            continue;
        }

        /* Allocate a slot for the file. */
        int slot = memfs_alloc_slot();
        if (slot < 0) {
            pr_warn("initrd: no free slot for '%s'\n", full_path);
            byte_ptr += file_size;
            continue;
        }

        strncpy(fs_files[slot].name, basename_buf, MAX_NAME_LEN - 1);
        fs_files[slot].name[MAX_NAME_LEN - 1] = '\0';
        fs_files[slot].exists     = 1;
        fs_files[slot].is_dir     = 0;
        fs_files[slot].mode       = S_IFREG | 0644;
        fs_files[slot].size       = file_size;
        fs_files[slot].parent_idx = parent_idx;
        fs_files[slot].num_children = 0;
        fs_offsets[slot] = 0;
        fs_flags[slot] = 0;

        if (file_size > MAX_FILE_SIZE) file_size = MAX_FILE_SIZE;
        memcpy(fs_files[slot].data, byte_ptr, file_size);

        memfs_add_child(parent_idx, slot);
        fs_num_files++;
        loaded++;
        pr_info("initrd:   %s (%u bytes)\n", full_path, file_size);

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
