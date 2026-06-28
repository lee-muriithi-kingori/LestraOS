/*
 * Lestra OS - Virtual File System Core
 * Copyright (c) 2026 lestramk.org
 */

#include <lestra/types.h>
#include <lestra/vfs.h>
#include <lestra/mm.h>
#include <lestra/printk.h>
#include <lestra/panic.h>

static struct mount mounts[MAX_MOUNTS];
static struct file_descriptor fd_table[MAX_OPEN_FILES];
static struct filesystem* fs_drivers[16];
static int fs_count = 0;

/* Root vnode */
static struct vnode* root_vnode = NULL;

void vfs_init(void) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mounts[i].root = NULL;
    }
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        fd_table[i].fd = -1;
    }
    fs_count = 0;
    
    pr_debug("VFS initialized\n");
}

void vfs_register_fs(struct filesystem* fs) {
    if (fs_count < 16) {
        fs_drivers[fs_count++] = fs;
        pr_debug("Registered filesystem: %s\n", fs->name);
    }
}

int vfs_mount(const char* source, const char* target, const char* fs_type) {
    /* Find filesystem driver */
    struct filesystem* fs = NULL;
    for (int i = 0; i < fs_count; i++) {
        if (strcmp(fs_drivers[i]->name, fs_type) == 0) {
            fs = fs_drivers[i];
            break;
        }
    }
    if (!fs) {
        pr_err("Unknown filesystem: %s\n", fs_type);
        return -ENODEV;
    }
    
    /* Find free mount slot */
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].root == NULL) {
            mounts[i].root = fs->mount(source);
            if (!mounts[i].root) return -EIO;
            
            strncpy(mounts[i].path, target, MAX_PATH_LEN);
            mounts[i].fs = fs;
            
            if (!root_vnode) {
                root_vnode = mounts[i].root;
            }
            
            pr_info("Mounted %s on %s\n", fs_type, target);
            return 0;
        }
    }
    return -EBUSY;
}

struct vnode* vfs_lookup(const char* path) {
    if (!root_vnode) return NULL;
    if (path[0] == '/') path++;
    
    /* Simple lookup - just return root for now */
    /* Full path traversal would be implemented here */
    if (path[0] == '\0' || (path[0] == '.' && path[1] == '\0')) {
        return root_vnode;
    }
    
    return NULL;
}

int vfs_open(const char* path, int flags) {
    struct vnode* node = vfs_lookup(path);
    if (!node) {
        if (flags & O_CREAT) {
            return vfs_create(path, 0644);
        }
        return -ENOTFOUND;
    }
    
    /* Find free fd */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (fd_table[i].fd == -1) {
            fd_table[i].fd = i;
            fd_table[i].flags = flags;
            fd_table[i].offset = 0;
            fd_table[i].node = node;
            node->refcount++;
            
            if (node->ops && node->ops->open) {
                node->ops->open(node, flags);
            }
            
            return i;
        }
    }
    return -EBUSY;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd].fd == -1) {
        return -EINVAL;
    }
    
    struct vnode* node = fd_table[fd].node;
    if (node && node->ops && node->ops->close) {
        node->ops->close(node);
    }
    if (node) node->refcount--;
    
    fd_table[fd].fd = -1;
    fd_table[fd].node = NULL;
    return 0;
}

ssize_t vfs_read(int fd, void* buf, size_t count) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd].fd == -1) {
        return -EINVAL;
    }
    
    struct vnode* node = fd_table[fd].node;
    if (!node || !node->ops || !node->ops->read) {
        return -ENOSYS;
    }
    
    ssize_t ret = node->ops->read(node, buf, count, fd_table[fd].offset);
    if (ret > 0) fd_table[fd].offset += ret;
    return ret;
}

ssize_t vfs_write(int fd, const void* buf, size_t count) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd].fd == -1) {
        return -EINVAL;
    }
    
    struct vnode* node = fd_table[fd].node;
    if (!node || !node->ops || !node->ops->write) {
        return -ENOSYS;
    }
    
    ssize_t ret = node->ops->write(node, buf, count, fd_table[fd].offset);
    if (ret > 0) fd_table[fd].offset += ret;
    return ret;
}

int vfs_create(const char* path, uint32_t mode) {
    (void)path;
    (void)mode;
    return -ENOSYS;  /* Placeholder */
}

int vfs_mkdir(const char* path, uint32_t mode) {
    (void)path;
    (void)mode;
    return -ENOSYS;
}

int vfs_readdir(int fd, struct dirent* entry) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd].fd == -1) {
        return -EINVAL;
    }
    
    struct vnode* node = fd_table[fd].node;
    if (!node || !node->ops || !node->ops->readdir) {
        return -ENOSYS;
    }
    
    int ret = node->ops->readdir(node, entry, (int)fd_table[fd].offset);
    if (ret == 0) fd_table[fd].offset++;
    return ret;
}

int vfs_stat(const char* path, struct stat* st) {
    struct vnode* node = vfs_lookup(path);
    if (!node) return -ENOTFOUND;
    
    st->mode = node->mode;
    st->size = node->size;
    return 0;
}

/* Simple string functions needed by VFS */
static int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

static char* strncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}
