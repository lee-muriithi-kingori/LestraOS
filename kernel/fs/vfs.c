/*
 * Lestra OS - Virtual File System
 * Copyright (c) 2026 lestramk.org
 *
 * Unified interface for filesystem operations.
 * Supports ext2, initrd, and procfs.
 */

#include <lestra/types.h>
#include <lestra/vfs.h>
#include <lestra/mm.h>
#include <lestra/printk.h>
#include <lestra/panic.h>

/* VFS Node - represents a file or directory */
struct vfs_node {
    char name[MAX_PATH_LEN];
    uint32_t type;          /* VNODE_FILE, VNODE_DIR, etc */
    uint32_t permissions;
    uint64_t size;
    uint64_t inode;
    struct vfs_ops* ops;
    struct vfs_node* parent;
    struct vfs_node* children;
    struct vfs_node* next;  /* Sibling in directory */
    void* private_data;
};

/* File descriptor table entry */
struct fd_entry {
    struct vfs_node* node;
    uint64_t offset;
    int flags;
    bool used;
};

static struct fd_entry fd_table[MAX_OPEN_FILES];
static struct vfs_node* root_node = NULL;

/* Mount points */
struct mount_point {
    char path[MAX_PATH_LEN];
    struct vfs_node* root;
};
static struct mount_point mounts[MAX_MOUNTS];

/* FIX #18: Add spinlock for fd_table and vfs state protection */
static volatile int vfs_lock = 0;

static inline void vfs_lock_acquire(void) {
    while (__sync_lock_test_and_set(&vfs_lock, 1));
}

static inline void vfs_lock_release(void) {
    __sync_lock_release(&vfs_lock);
}

void vfs_init(void) {
    memset(fd_table, 0, sizeof(fd_table));
    memset(mounts, 0, sizeof(mounts));

    /* Create root directory */
    root_node = (struct vfs_node*)kcalloc(1, sizeof(struct vfs_node));
    if (!root_node) {
        panic("VFS: Failed to create root node");
    }

    strncpy(root_node->name, "/", MAX_PATH_LEN - 1);
    root_node->name[MAX_PATH_LEN - 1] = '\0';
    root_node->type = VNODE_DIR;
    root_node->permissions = 0755;
    root_node->size = 0;
    root_node->inode = 0;
    root_node->parent = root_node;
    root_node->children = NULL;
    root_node->next = NULL;
    root_node->private_data = NULL;

    pr_info("VFS initialized\n");
}

void vfs_register(const char* mountpoint, struct vfs_ops* ops) {
    if (!mountpoint || !ops) return;

    vfs_lock_acquire();

    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].root) {
            strncpy(mounts[i].path, mountpoint, MAX_PATH_LEN - 1);
            mounts[i].path[MAX_PATH_LEN - 1] = '\0';
            mounts[i].root = root_node;
            break;
        }
    }

    vfs_lock_release();
}

/* FIX #15: Implement proper path traversal */
static struct vfs_node* vfs_lookup_path(const char* path) {
    if (!path || path[0] != '/') return NULL;

    /* Handle root */
    if (path[1] == '\0') return root_node;

    /* Walk path components */
    char path_buf[MAX_PATH_LEN];
    if (strlen(path) >= MAX_PATH_LEN) return NULL;
    strncpy(path_buf, path, MAX_PATH_LEN - 1);
    path_buf[MAX_PATH_LEN - 1] = '\0';

    struct vfs_node* current = root_node;
    char* component = path_buf + 1; /* Skip leading / */
    char* rest;

    while (component && *component) {
        /* Find next / */
        rest = component;
        while (*rest && *rest != '/') rest++;
        if (*rest) {
            *rest = '\0';
            rest++;
        } else {
            rest = NULL;
        }

        /* Handle . and .. */
        if (strcmp(component, ".") == 0) {
            component = rest;
            continue;
        }
        if (strcmp(component, "..") == 0) {
            if (current != root_node) {
                current = current->parent;
            }
            component = rest;
            continue;
        }

        /* Search children */
        struct vfs_node* child = current->children;
        while (child) {
            if (strcmp(child->name, component) == 0) {
                break;
            }
            child = child->next;
        }

        if (!child) return NULL; /* Component not found */
        current = child;
        component = rest;
    }

    return current;
}

struct vfs_node* vfs_lookup(const char* path) {
    vfs_lock_acquire();
    struct vfs_node* result = vfs_lookup_path(path);
    vfs_lock_release();
    return result;
}

int vfs_open(const char* path, int flags) {
    if (!path || path[0] != '/') return -EINVAL;

    vfs_lock_acquire();

    struct vfs_node* node = vfs_lookup_path(path);
    if (!node) {
        /* Try to create if O_CREAT */
        if (flags & O_CREAT) {
            /* Extract parent path and filename */
            char parent_path[MAX_PATH_LEN];
            strncpy(parent_path, path, MAX_PATH_LEN - 1);
            parent_path[MAX_PATH_LEN - 1] = '\0';

            char* last_slash = strrchr(parent_path, '/');
            if (!last_slash || last_slash == parent_path) {
                vfs_lock_release();
                return -EINVAL;
            }

            char filename[MAX_PATH_LEN];
            strncpy(filename, last_slash + 1, MAX_PATH_LEN - 1);
            filename[MAX_PATH_LEN - 1] = '\0';
            *last_slash = '\0';

            struct vfs_node* parent = vfs_lookup_path(parent_path);
            if (!parent || parent->type != VNODE_DIR) {
                vfs_lock_release();
                return -ENOENT;
            }

            node = (struct vfs_node*)kcalloc(1, sizeof(struct vfs_node));
            if (!node) {
                vfs_lock_release();
                return -ENOMEM;
            }

            strncpy(node->name, filename, MAX_PATH_LEN - 1);
            node->name[MAX_PATH_LEN - 1] = '\0';
            node->type = VNODE_FILE;
            node->permissions = 0644;
            node->parent = parent;
            node->next = parent->children;
            parent->children = node;
        } else {
            vfs_lock_release();
            return -ENOENT;
        }
    }

    /* Check permissions */
    if ((flags & O_WRONLY) && !(node->permissions & 0222)) {
        vfs_lock_release();
        return -EACCES;
    }

    /* Allocate file descriptor */
    int fd = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!fd_table[i].used) {
            fd = i;
            fd_table[i].node = node;
            fd_table[i].offset = (flags & O_APPEND) ? node->size : 0;
            fd_table[i].flags = flags;
            fd_table[i].used = true;
            break;
        }
    }

    if (fd < 0) {
        vfs_lock_release();
        return -ENFILE;
    }

    /* Call filesystem-specific open if available */
    if (node->ops && node->ops->open) {
        int ret = node->ops->open(node, flags);
        if (ret < 0) {
            /* Clean up fd on error */
            fd_table[fd].used = false;
            fd_table[fd].node = NULL;
            vfs_lock_release();
            return ret;
        }
    }

    vfs_lock_release();
    return fd;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    vfs_lock_acquire();

    if (!fd_table[fd].used) {
        vfs_lock_release();
        return -EBADF;
    }

    struct vfs_node* node = fd_table[fd].node;

    if (node && node->ops && node->ops->close) {
        node->ops->close(node);
    }

    fd_table[fd].used = false;
    fd_table[fd].node = NULL;
    fd_table[fd].offset = 0;

    vfs_lock_release();
    return 0;
}

int64_t vfs_read(int fd, void* buf, uint64_t count) {
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;
    if (!buf) return -EFAULT;

    vfs_lock_acquire();

    if (!fd_table[fd].used) {
        vfs_lock_release();
        return -EBADF;
    }

    struct vfs_node* node = fd_table[fd].node;
    if (!node || !node->ops || !node->ops->read) {
        vfs_lock_release();
        return -EINVAL;
    }

    int64_t ret = node->ops->read(node, buf, count, fd_table[fd].offset);
    if (ret > 0) {
        fd_table[fd].offset += ret;
    }

    vfs_lock_release();
    return ret;
}

int64_t vfs_write(int fd, const void* buf, uint64_t count) {
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;
    if (!buf) return -EFAULT;

    vfs_lock_acquire();

    if (!fd_table[fd].used) {
        vfs_lock_release();
        return -EBADF;
    }

    struct vfs_node* node = fd_table[fd].node;
    if (!node || !node->ops || !node->ops->write) {
        vfs_lock_release();
        return -EINVAL;
    }

    int64_t ret = node->ops->write(node, buf, count, fd_table[fd].offset);
    if (ret > 0) {
        fd_table[fd].offset += ret;
        if (fd_table[fd].offset > node->size) {
            node->size = fd_table[fd].offset;
        }
    }

    vfs_lock_release();
    return ret;
}

int vfs_mkdir(const char* path, uint32_t mode) {
    if (!path || path[0] != '/') return -EINVAL;

    vfs_lock_acquire();

    /* Check if already exists */
    if (vfs_lookup_path(path)) {
        vfs_lock_release();
        return -EEXIST;
    }

    /* Get parent directory */
    char parent_path[MAX_PATH_LEN];
    strncpy(parent_path, path, MAX_PATH_LEN - 1);
    parent_path[MAX_PATH_LEN - 1] = '\0';

    char* last_slash = strrchr(parent_path, '/');
    if (!last_slash || last_slash == parent_path) {
        vfs_lock_release();
        return -EINVAL;
    }

    char dirname[MAX_PATH_LEN];
    strncpy(dirname, last_slash + 1, MAX_PATH_LEN - 1);
    dirname[MAX_PATH_LEN - 1] = '\0';
    *last_slash = '\0';

    struct vfs_node* parent = vfs_lookup_path(parent_path);
    if (!parent || parent->type != VNODE_DIR) {
        vfs_lock_release();
        return -ENOENT;
    }

    /* Create directory node */
    struct vfs_node* node = (struct vfs_node*)kcalloc(1, sizeof(struct vfs_node));
    if (!node) {
        vfs_lock_release();
        return -ENOMEM;
    }

    strncpy(node->name, dirname, MAX_PATH_LEN - 1);
    node->name[MAX_PATH_LEN - 1] = '\0';
    node->type = VNODE_DIR;
    node->permissions = mode;
    node->parent = parent;
    node->next = parent->children;
    parent->children = node;

    vfs_lock_release();
    return 0;
}

int vfs_stat(const char* path, struct stat* st) {
    if (!path || !st) return -EINVAL;

    vfs_lock_acquire();

    struct vfs_node* node = vfs_lookup_path(path);
    if (!node) {
        vfs_lock_release();
        return -ENOENT;
    }

    memset(st, 0, sizeof(struct stat));
    st->st_size = node->size;
    st->st_mode = node->permissions;
    st->st_ino = node->inode;
    if (node->type == VNODE_DIR) st->st_mode |= S_IFDIR;
    else if (node->type == VNODE_FILE) st->st_mode |= S_IFREG;

    vfs_lock_release();
    return 0;
}

int vfs_getdents(int fd, struct dirent* dirp, uint32_t count) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || !dirp) return -EINVAL;

    vfs_lock_acquire();

    if (!fd_table[fd].used || !fd_table[fd].node) {
        vfs_lock_release();
        return -EBADF;
    }

    struct vfs_node* node = fd_table[fd].node;
    if (node->type != VNODE_DIR) {
        vfs_lock_release();
        return -ENOTDIR;
    }

    /* TODO: Proper directory enumeration with offset */
    uint32_t written = 0;
    struct vfs_node* child = node->children;

    while (child && written + sizeof(struct dirent) <= count) {
        strncpy(dirp->d_name, child->name, sizeof(dirp->d_name) - 1);
        dirp->d_name[sizeof(dirp->d_name) - 1] = '\0';
        dirp->d_ino = child->inode;
        dirp->d_type = (child->type == VNODE_DIR) ? 4 : 8;

        written += sizeof(struct dirent);
        dirp++;
        child = child->next;
    }

    vfs_lock_release();
    return written;
}
