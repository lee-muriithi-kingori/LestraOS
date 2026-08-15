/*
 * Lestra OS - tarfs: read-only tar filesystem for the rootfs
 * Parses POSIX ustar headers, indexes regular files + symlinks,
 * resolves symlinks at open time.
 */
#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/vfs.h>
#include <string.h>

#define MAX_TARFS_ENTRIES 8192
#define MAX_TARFS_OPEN 128
#define TAR_BLOCK_SIZE 512
#define TARFS_FD_BASE 200
#define MAX_SYMLINK_DEPTH 10

struct tarfs_entry {
    int used;
    char name[256];
    uint64_t offset;
    uint64_t size;
    int is_symlink;
    char link_target[256];
};

struct tarfs_open {
    int used;
    int entry_idx;
    uint64_t pos;
};

static struct tarfs_entry tarfs_entries[MAX_TARFS_ENTRIES];
static struct tarfs_open tarfs_opens[MAX_TARFS_OPEN];
static const uint8_t* tarfs_blob = NULL;
static uint64_t tarfs_blob_size = 0;
static int tarfs_mounted_flag = 0;
static int tarfs_file_count_val = 0;

static void normalize_name(const char* src, char* dst, size_t dstsz) {
    if (!src || !dst || dstsz == 0) return;
    if (src[0] == '.' && src[1] == '/') src += 2;
    if (src[0] == '/') src++;
    strncpy(dst, src, dstsz - 1);
    dst[dstsz - 1] = '\0';
    if (dst[0] != '/') {
        memmove(dst + 1, dst, strlen(dst) + 1);
        dst[0] = '/';
    }
}

static int find_entry(const char* path) {
    char norm[256];
    normalize_name(path, norm, sizeof(norm));
    for (int i = 0; i < MAX_TARFS_ENTRIES; i++) {
        if (tarfs_entries[i].used && strcmp(tarfs_entries[i].name, norm) == 0)
            return i;
    }
    return -1;
}

int tarfs_resolve_symlink(const char* path, char* resolved, size_t rsz) {
    char current[256];
    normalize_name(path, current, sizeof(current));
    for (int depth = 0; depth < MAX_SYMLINK_DEPTH; depth++) {
        int idx = find_entry(current);
        if (idx < 0) return -1;
        if (!tarfs_entries[idx].is_symlink) {
            strncpy(resolved, current, rsz - 1);
            resolved[rsz - 1] = '\0';
            return 0;
        }
        const char* target = tarfs_entries[idx].link_target;
        if (target[0] == '/') {
            strncpy(current, target, sizeof(current) - 1);
            current[sizeof(current) - 1] = '\0';
        } else {
            char dir[256];
            strncpy(dir, current, sizeof(dir) - 1);
            dir[sizeof(dir) - 1] = '\0';
            char* slash = strrchr(dir, '/');
            if (slash) slash[1] = '\0';
            else strcpy(dir, "/");
            size_t dl = strlen(dir);
            size_t tl = strlen(target);
            if (dl + tl >= sizeof(current)) return -1;
            strcpy(current, dir);
            strcat(current, target);
        }
    }
    return -1;
}

int tarfs_readlink(const char* path, char* buf, size_t bufsz) {
    int idx = find_entry(path);
    if (idx < 0 || !tarfs_entries[idx].is_symlink) return -1;
    strncpy(buf, tarfs_entries[idx].link_target, bufsz - 1);
    buf[bufsz - 1] = '\0';
    return 0;
}

int tarfs_open(const char* path) {
    char resolved[256];
    if (tarfs_resolve_symlink(path, resolved, sizeof(resolved)) == 0) {
        path = resolved;
    }
    int idx = find_entry(path);
    if (idx < 0) return -1;
    if (tarfs_entries[idx].is_symlink) return -1;
    for (int i = 0; i < MAX_TARFS_OPEN; i++) {
        if (!tarfs_opens[i].used) {
            tarfs_opens[i].used = 1;
            tarfs_opens[i].entry_idx = idx;
            tarfs_opens[i].pos = 0;
            return i + TARFS_FD_BASE;
        }
    }
    return -1;
}

int tarfs_close(int fd) {
    fd -= TARFS_FD_BASE;
    if (fd < 0 || fd >= MAX_TARFS_OPEN) return -1;
    tarfs_opens[fd].used = 0;
    return 0;
}

ssize_t tarfs_read(int fd, void* buf, size_t count) {
    fd -= TARFS_FD_BASE;
    if (fd < 0 || fd >= MAX_TARFS_OPEN || !tarfs_opens[fd].used) return -1;
    struct tarfs_open* o = &tarfs_opens[fd];
    struct tarfs_entry* e = &tarfs_entries[o->entry_idx];
    if (o->pos >= e->size) return 0;
    size_t avail = e->size - o->pos;
    if (count > avail) count = avail;
    memcpy(buf, tarfs_blob + e->offset + o->pos, count);
    o->pos += count;
    return (ssize_t)count;
}

int tarfs_stat(const char* path, struct stat* st) {
    char resolved[256];
    if (tarfs_resolve_symlink(path, resolved, sizeof(resolved)) == 0)
        path = resolved;
    int idx = find_entry(path);
    if (idx < 0) return -1;
    if (st) {
        memset(st, 0, sizeof(*st));
        st->size = tarfs_entries[idx].size;
        st->mode = 0100644;
    }
    return 0;
}

int tarfs_is_mounted(void) { return tarfs_mounted_flag; }
int tarfs_file_count(void) { return tarfs_file_count_val; }

void tarfs_init(const void* blob, uint64_t size) {
    if (!blob || size < TAR_BLOCK_SIZE) {
        pr_warn("tarfs: invalid blob\n");
        return;
    }
    tarfs_blob = (const uint8_t*)blob;
    tarfs_blob_size = size;
    memset(tarfs_entries, 0, sizeof(tarfs_entries));
    memset(tarfs_opens, 0, sizeof(tarfs_opens));

    int regular = 0, symlinks = 0, skipped = 0;
    const uint8_t* ptr = blob;
    const uint8_t* end = blob + size;

    while (ptr + TAR_BLOCK_SIZE <= end) {
        const char* header = (const char*)ptr;
        if (header[0] == '\0') break;

        char typeflag = header[156];
        if (typeflag == 'x' || typeflag == 'g') {
            /* Skip pax/gnu extended header */
            char size_str[12];
            memcpy(size_str, header + 124, 11);
            size_str[11] = '\0';
            uint64_t skip = 0;
            for (int i = 0; i < 11 && size_str[i] >= '0' && size_str[i] <= '7'; i++)
                skip = skip * 8 + (size_str[i] - '0');
            skip = (skip + TAR_BLOCK_SIZE - 1) & ~((uint64_t)TAR_BLOCK_SIZE - 1);
            ptr += TAR_BLOCK_SIZE + skip;
            continue;
        }

        int is_regular = (typeflag == '0' || typeflag == '\0' || typeflag == '7');
        int is_symlink = (typeflag == '2');
        if (!is_regular && !is_symlink) {
            char size_str[12];
            memcpy(size_str, header + 124, 11);
            size_str[11] = '\0';
            uint64_t skip = 0;
            for (int i = 0; i < 11 && size_str[i] >= '0' && size_str[i] <= '7'; i++)
                skip = skip * 8 + (size_str[i] - '0');
            skip = (skip + TAR_BLOCK_SIZE - 1) & ~((uint64_t)TAR_BLOCK_SIZE - 1);
            ptr += TAR_BLOCK_SIZE + skip;
            skipped++;
            continue;
        }

        char name[256];
        normalize_name(header, name, sizeof(name));
        if (name[0] == '\0' || strcmp(name, "/") == 0) {
            ptr += TAR_BLOCK_SIZE;
            continue;
        }

        char size_str[12];
        memcpy(size_str, header + 124, 11);
        size_str[11] = '\0';
        uint64_t file_size = 0;
        for (int i = 0; i < 11 && size_str[i] >= '0' && size_str[i] <= '7'; i++)
            file_size = file_size * 8 + (size_str[i] - '0');

        uint64_t data_offset = (uint64_t)(ptr - (const uint8_t*)blob) + TAR_BLOCK_SIZE;
        uint64_t padded = (file_size + TAR_BLOCK_SIZE - 1) & ~((uint64_t)TAR_BLOCK_SIZE - 1);

        for (int i = 0; i < MAX_TARFS_ENTRIES; i++) {
            if (!tarfs_entries[i].used) {
                tarfs_entries[i].used = 1;
                strncpy(tarfs_entries[i].name, name, 255);
                tarfs_entries[i].name[255] = '\0';
                tarfs_entries[i].offset = data_offset;
                tarfs_entries[i].size = file_size;
                tarfs_entries[i].is_symlink = is_symlink ? 1 : 0;
                if (is_symlink) {
                    strncpy(tarfs_entries[i].link_target, header + 157, 255);
                    tarfs_entries[i].link_target[255] = '\0';
                    tarfs_entries[i].offset = 0;
                    tarfs_entries[i].size = 0;
                    symlinks++;
                } else {
                    regular++;
                }
                break;
            }
        }

        ptr += TAR_BLOCK_SIZE + padded;
    }

    tarfs_mounted_flag = 1;
    tarfs_file_count_val = regular + symlinks;
    pr_info("tarfs: mounted (%llu KB blob, %d entries: %d regular + %d symlinks, %d skipped)\n",
            (unsigned long long)size / 1024, tarfs_file_count_val, regular, symlinks, skipped);
}
