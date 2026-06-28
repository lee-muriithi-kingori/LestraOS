/*
 * Lestra OS - Initial Ramdisk
 * Copyright (c) 2026 lestramk.org
 *
 * Simple tar-like initrd format for boot-time files.
 */

#include <lestra/types.h>
#include <lestra/vfs.h>
#include <lestra/mm.h>
#include <lestra/printk.h>

/* initrd file header */
struct initrd_header {
    char magic[4];       /* "LRD\0" */
    uint32_t num_files;
    uint32_t data_offset;
};

struct initrd_file {
    char name[MAX_NAME_LEN];
    uint32_t offset;
    uint32_t size;
    uint16_t mode;
    uint16_t type;
};

static struct initrd_header* rd_header = NULL;
static struct initrd_file* rd_files = NULL;
static uint8_t* rd_data = NULL;

/* initrd vnode operations */
static int initrd_open(struct vnode* node, int flags) {
    (void)flags;
    (void)node;
    return 0;
}

static int initrd_close(struct vnode* node) {
    (void)node;
    return 0;
}

static ssize_t initrd_read(struct vnode* node, void* buf, size_t count, off_t offset) {
    struct initrd_file* file = (struct initrd_file*)node->private_data;
    
    if (offset >= file->size) return 0;
    if (offset + count > file->size) count = file->size - offset;
    
    uint8_t* src = rd_data + file->offset + offset;
    uint8_t* dst = (uint8_t*)buf;
    for (size_t i = 0; i < count; i++) {
        dst[i] = src[i];
    }
    
    return count;
}

static ssize_t initrd_write(struct vnode* node, const void* buf, size_t count, off_t offset) {
    (void)node;
    (void)buf;
    (void)count;
    (void)offset;
    return -EROFS;  /* Read-only */
}

static int initrd_readdir(struct vnode* node, struct dirent* entry, int index) {
    (void)node;
    if (index < 0 || index >= (int)rd_header->num_files) return -1;
    
    entry->inode = index;
    entry->type = FT_REGULAR;
    char* name = rd_files[index].name;
    for (int i = 0; i < MAX_NAME_LEN; i++) {
        entry->name[i] = name[i];
    }
    entry->reclen = sizeof(struct dirent);
    
    return 0;
}

static struct file_ops initrd_fops = {
    .open = initrd_open,
    .close = initrd_close,
    .read = initrd_read,
    .write = initrd_write,
    .readdir = initrd_readdir,
};

/* initrd root vnode */
static struct vnode initrd_root;

static struct vnode* initrd_mount(const char* source) {
    (void)source;
    
    initrd_root.inode = 0;
    initrd_root.refcount = 1;
    initrd_root.type = FT_DIRECTORY;
    initrd_root.mode = 0755;
    initrd_root.size = 0;
    initrd_root.ops = &initrd_fops;
    initrd_root.private_data = NULL;
    initrd_root.parent = NULL;
    initrd_root.mount = NULL;
    
    return &initrd_root;
}

static int initrd_unmount(struct mount* mount) {
    (void)mount;
    return 0;
}

static struct filesystem initrd_fs = {
    .name = "initrd",
    .mount = initrd_mount,
    .unmount = initrd_unmount,
};

void initrd_load(void* data, size_t size) {
    (void)size;
    
    rd_header = (struct initrd_header*)data;
    
    /* Verify magic */
    if (rd_header->magic[0] != 'L' || rd_header->magic[1] != 'R' ||
        rd_header->magic[2] != 'D' || rd_header->magic[3] != 0) {
        pr_warn("initrd: Invalid magic, creating empty ramdisk\n");
        return;
    }
    
    rd_files = (struct initrd_file*)((uintptr_t)data + sizeof(struct initrd_header));
    rd_data = (uint8_t*)((uintptr_t)data + rd_header->data_offset);
    
    pr_info("initrd: Loaded %u files\n", rd_header->num_files);
    for (uint32_t i = 0; i < rd_header->num_files; i++) {
        pr_debug("  %s (%u bytes)\n", rd_files[i].name, rd_files[i].size);
    }
}

struct filesystem* initrd_get_fs(void) {
    return &initrd_fs;
}
