/*
 * Lestra OS - ext2 filesystem driver
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * A minimal ext2 filesystem driver. It can:
 *   - Mount an ext2 filesystem on a block device (AHCI SATA drive)
 *   - Read files and directories
 *   - Create and delete files and directories
 *   - Write data to files
 *
 * Limitations:
 *   - No triple-indirect block support (direct + single/double indirect)
 *   - Block size must be 1024 bytes (the standard for small filesystems)
 *   - No symbolic links, no special files
 *
 * The ext2 superblock is at byte offset 1024 from the start of the disk.
 * Block group descriptors follow the superblock's block.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <lestra/timer.h>
#include <string.h>

extern int ahci_read_sectors(uint64_t lba, uint32_t count, void* buf);
extern int ahci_write_sectors(uint64_t lba, uint32_t count, const void* buf);
extern int ahci_has_drive(void);

struct ext2_superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
} __packed;

struct ext2_bgd {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __packed;

struct ext2_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __packed;

struct ext2_dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __packed;

static int ext2_mounted = 0;
static uint32_t ext2_block_size = 1024;
static uint32_t ext2_inodes_per_group;
static uint32_t ext2_blocks_per_group;
static uint16_t ext2_inode_size;
static struct ext2_superblock ext2_sb;

static int disk_read(uint64_t offset, void* buf, uint32_t count) {
    if (!ahci_has_drive()) return 0;
    uint64_t lba = offset / 512;
    uint32_t sectors = (count + 511) / 512;
    if (sectors > 8) sectors = 8;
    return ahci_read_sectors(lba, sectors, buf) > 0;
}

static int disk_write(uint64_t offset, const void* buf, uint32_t count) {
    if (!ahci_has_drive()) return 0;
    uint64_t lba = offset / 512;
    uint32_t sectors = (count + 511) / 512;
    if (sectors > 8) sectors = 8;
    return ahci_write_sectors(lba, sectors, buf) > 0;
}

static int disk_write_bgd(int group, const struct ext2_bgd* bgd) {
    uint64_t bgd_start;
    if (ext2_block_size == 1024) {
        bgd_start = 2 * ext2_block_size;
    } else {
        bgd_start = 1 * ext2_block_size;
    }
    uint64_t offset = bgd_start + group * sizeof(struct ext2_bgd);
    static uint8_t sector_buf[512];
    uint64_t sector = offset / 512;
    uint32_t intra = (uint32_t)(offset % 512);
    if (!ahci_read_sectors(sector, 1, sector_buf)) return 0;
    memcpy(sector_buf + intra, bgd, sizeof(struct ext2_bgd));
    return ahci_write_sectors(sector, 1, sector_buf) > 0;
}

static int ext2_read_block(uint32_t block_num, void* buf) {
    uint64_t offset = (uint64_t)block_num * ext2_block_size;
    return disk_read(offset, buf, ext2_block_size);
}

int ext2_mount(void) {
    if (!ahci_has_drive()) {
        pr_info("ext2: no disk drive available\n");
        return 0;
    }

    static uint8_t sb_buf[1024];
    if (!disk_read(1024, sb_buf, 1024)) {
        pr_warn("ext2: failed to read superblock\n");
        return 0;
    }

    memcpy(&ext2_sb, sb_buf, sizeof(ext2_sb));

    if (ext2_sb.s_magic != 0xEF53) {
        pr_warn("ext2: bad magic 0x%x (expected 0xEF53)\n",
                (unsigned)ext2_sb.s_magic);
        return 0;
    }

    ext2_block_size = 1024u << ext2_sb.s_log_block_size;
    ext2_inodes_per_group = ext2_sb.s_inodes_per_group;
    ext2_blocks_per_group = ext2_sb.s_blocks_per_group;
    ext2_inode_size = ext2_sb.s_inode_size;
    if (ext2_inode_size == 0) ext2_inode_size = 128;

    pr_info("ext2: mounted - %u blocks, %u inodes, block_size=%u, inode_size=%u\n",
            (unsigned)ext2_sb.s_blocks_count,
            (unsigned)ext2_sb.s_inodes_count,
            (unsigned)ext2_block_size,
            (unsigned)ext2_inode_size);

    if (ext2_sb.s_volume_name[0]) {
        pr_info("ext2: volume name '%.16s'\n", ext2_sb.s_volume_name);
    }

    ext2_mounted = 1;
    return 1;
}

int ext2_is_mounted(void) {
    return ext2_mounted;
}

/* Forward declarations (defined later in this file) */
static int ext2_read_inode(uint32_t inode_num, struct ext2_inode* out);
uint32_t ext2_resolve_path(const char* path);

/* ext2_get_inode_mode: return the i_mode field of the inode at `path`.
 * This allows callers (like the ext2_shim) to determine whether a
 * path is a regular file, directory, etc. without reading the entire
 * inode. Returns 0 if the path doesn't exist or the filesystem isn't
 * mounted. */
uint16_t ext2_get_inode_mode(const char* path) {
    if (!ext2_mounted || !path) return 0;
    uint32_t inode_num = ext2_resolve_path(path);
    if (inode_num == 0) return 0;
    struct ext2_inode inode;
    if (!ext2_read_inode(inode_num, &inode)) return 0;
    return inode.i_mode;
}

static int ext2_read_inode(uint32_t inode_num, struct ext2_inode* out) {
    if (!ext2_mounted || inode_num == 0) return 0;

    uint32_t group = (inode_num - 1) / ext2_inodes_per_group;
    uint32_t index = (inode_num - 1) % ext2_inodes_per_group;

    uint32_t bgd_block;
    if (ext2_block_size == 1024) {
        bgd_block = 2;
    } else {
        bgd_block = 1;
    }
    uint32_t bgd_offset = bgd_block * ext2_block_size + group * sizeof(struct ext2_bgd);

    static uint8_t block_buf[4096];
    if (!disk_read(bgd_block * ext2_block_size, block_buf, ext2_block_size)) {
        return 0;
    }

    struct ext2_bgd* bgd = (struct ext2_bgd*)&block_buf[group * sizeof(struct ext2_bgd)];
    uint32_t inode_table_block = bgd->bg_inode_table;

    uint64_t inode_offset = (uint64_t)inode_table_block * ext2_block_size
                          + index * ext2_inode_size;
    static uint8_t inode_buf[256];
    if (!disk_read(inode_offset, inode_buf, ext2_inode_size)) {
        return 0;
    }

    memcpy(out, inode_buf, sizeof(struct ext2_inode));
    return 1;
}

static uint32_t ext2_get_inode_block(struct ext2_inode* inode, uint32_t logical_block) {
    uint32_t bpb = ext2_block_size / 4;

    if (logical_block < 12) {
        return inode->i_block[logical_block];
    }

    logical_block -= 12;
    if (logical_block < bpb) {
        if (inode->i_block[12] == 0) return 0;
        static uint32_t ptr[1024];
        if (!ext2_read_block(inode->i_block[12], ptr)) return 0;
        return ptr[logical_block];
    }

    logical_block -= bpb;
    if (logical_block < bpb * bpb) {
        if (inode->i_block[13] == 0) return 0;
        static uint32_t ptr[1024];
        if (!ext2_read_block(inode->i_block[13], ptr)) return 0;
        uint32_t idx1 = logical_block / bpb;
        uint32_t idx2 = logical_block % bpb;
        if (ptr[idx1] == 0) return 0;
        uint32_t blk = ptr[idx1];
        if (!ext2_read_block(blk, ptr)) return 0;
        return ptr[idx2];
    }

    logical_block -= bpb * bpb;
    if (inode->i_block[14] == 0) return 0;
    static uint32_t ptr[1024];
    if (!ext2_read_block(inode->i_block[14], ptr)) return 0;
    uint32_t idx1 = logical_block / (bpb * bpb);
    uint32_t rem = logical_block % (bpb * bpb);
    uint32_t idx2 = rem / bpb;
    uint32_t idx3 = rem % bpb;
    if (ptr[idx1] == 0) return 0;
    uint32_t blk = ptr[idx1];
    if (!ext2_read_block(blk, ptr)) return 0;
    if (ptr[idx2] == 0) return 0;
    blk = ptr[idx2];
    if (!ext2_read_block(blk, ptr)) return 0;
    return ptr[idx3];
}

static uint32_t ext2_find_dirent(struct ext2_inode* dir_inode, const char* name) {
    if (!ext2_mounted) return 0;

    static uint8_t block_buf[4096];
    uint32_t bytes_walked = 0;
    uint32_t logical_block = 0;

    while (bytes_walked < dir_inode->i_size) {
        uint32_t phys = ext2_get_inode_block(dir_inode, logical_block);
        if (phys == 0) break;
        if (!ext2_read_block(phys, block_buf)) break;

        uint32_t offset = 0;
        while (offset < ext2_block_size && bytes_walked + offset < dir_inode->i_size) {
            struct ext2_dirent* de = (struct ext2_dirent*)&block_buf[offset];
            if (de->rec_len == 0) break;

            if (de->inode != 0 && de->name_len > 0) {
                int match = 1;
                for (int j = 0; j < de->name_len; j++) {
                    if (name[j] != de->name[j]) { match = 0; break; }
                }
                if (match && name[de->name_len] == '\0') {
                    return de->inode;
                }
            }
            offset += de->rec_len;
        }
        bytes_walked += ext2_block_size;
        logical_block++;
    }
    return 0;
}

uint32_t ext2_resolve_path(const char* path) {
    if (!ext2_mounted || !path || path[0] != '/') return 0;

    uint32_t inode_num = 2;
    struct ext2_inode inode;

    if (!ext2_read_inode(inode_num, &inode)) return 0;

    if (path[1] == '\0') return inode_num;

    const char* p = path + 1;
    while (*p) {
        char name[256];
        int len = 0;
        while (*p && *p != '/' && len < 255) {
            name[len++] = *p++;
        }
        name[len] = '\0';
        if (*p == '/') p++;

        if (len == 0) continue;

        inode_num = ext2_find_dirent(&inode, name);
        if (inode_num == 0) return 0;

        if (!ext2_read_inode(inode_num, &inode)) return 0;
    }

    return inode_num;
}

int ext2_read_file(const char* path, void* buf, uint32_t bufsize) {
    if (!ext2_mounted) return 0;

    uint32_t inode_num = ext2_resolve_path(path);
    if (inode_num == 0) {
        pr_warn("ext2: file '%s' not found\n", path);
        return 0;
    }

    struct ext2_inode inode;
    if (!ext2_read_inode(inode_num, &inode)) return 0;

    if ((inode.i_mode & 0xF000) != 0x8000) {
        pr_warn("ext2: '%s' is not a regular file\n", path);
        return 0;
    }

    uint32_t file_size = inode.i_size;
    if (file_size > bufsize) file_size = bufsize;

    static uint8_t block_buf[4096];
    uint32_t bytes_read = 0;
    while (bytes_read < file_size) {
        uint32_t logical_block = bytes_read / ext2_block_size;
        uint32_t phys = ext2_get_inode_block(&inode, logical_block);
        if (phys == 0) break;
        if (!ext2_read_block(phys, block_buf)) break;

        uint32_t block_offset = bytes_read % ext2_block_size;
        uint32_t to_copy = ext2_block_size - block_offset;
        if (to_copy > file_size - bytes_read) to_copy = file_size - bytes_read;
        memcpy((uint8_t*)buf + bytes_read, block_buf + block_offset, to_copy);
        bytes_read += to_copy;
    }

    return (int)bytes_read;
}

void ext2_list_root(void (*callback)(const char* name, uint32_t inode, uint8_t type)) {
    if (!ext2_mounted) return;

    struct ext2_inode inode;
    if (!ext2_read_inode(2, &inode)) return;

    static uint8_t block_buf[4096];
    uint32_t bytes_walked = 0;
    uint32_t logical_block = 0;

    while (bytes_walked < inode.i_size) {
        uint32_t phys = ext2_get_inode_block(&inode, logical_block);
        if (phys == 0) break;
        if (!ext2_read_block(phys, block_buf)) break;

        uint32_t offset = 0;
        while (offset < ext2_block_size && bytes_walked + offset < inode.i_size) {
            struct ext2_dirent* de = (struct ext2_dirent*)&block_buf[offset];
            if (de->rec_len == 0) break;

            if (de->inode != 0 && de->name_len > 0) {
                char name[256];
                int len = de->name_len < 255 ? de->name_len : 255;
                memcpy(name, de->name, len);
                name[len] = '\0';
                if (callback) callback(name, de->inode, de->file_type);
            }
            offset += de->rec_len;
        }
        bytes_walked += ext2_block_size;
        logical_block++;
    }
}

/* ext2_list_dir: list the contents of ANY ext2 directory (not just root).
 * Resolves `path` to an inode, then walks its data blocks and calls
 * the callback for each directory entry. This is a generalization of
 * ext2_list_root() that works for any path, e.g. "/etc", "/usr/bin". */
void ext2_list_dir(const char* path, void (*callback)(const char* name, uint32_t inode, uint8_t type)) {
    if (!ext2_mounted || !path || !callback) return;

    /* Resolve path to inode number. */
    uint32_t inode_num = ext2_resolve_path(path);
    if (inode_num == 0) {
        pr_warn("ext2: dir '%s' not found\n", path);
        return;
    }

    struct ext2_inode inode;
    if (!ext2_read_inode(inode_num, &inode)) return;

    /* Verify it's actually a directory. */
    if ((inode.i_mode & 0xF000) != 0x4000) {
        pr_warn("ext2: '%s' is not a directory\n", path);
        return;
    }

    static uint8_t block_buf[4096];
    uint32_t bytes_walked = 0;
    uint32_t logical_block = 0;

    while (bytes_walked < inode.i_size) {
        uint32_t phys = ext2_get_inode_block(&inode, logical_block);
        if (phys == 0) break;
        if (!ext2_read_block(phys, block_buf)) break;

        uint32_t offset = 0;
        while (offset < ext2_block_size && bytes_walked + offset < inode.i_size) {
            struct ext2_dirent* de = (struct ext2_dirent*)&block_buf[offset];
            if (de->rec_len == 0) break;

            if (de->inode != 0 && de->name_len > 0) {
                char name[256];
                int len = de->name_len < 255 ? de->name_len : 255;
                memcpy(name, de->name, len);
                name[len] = '\0';
                callback(name, de->inode, de->file_type);
            }
            offset += de->rec_len;
        }
        bytes_walked += ext2_block_size;
        logical_block++;
    }
}

/* ========================================================================
 * WRITE SUPPORT
 * ======================================================================== */

static uint32_t ext2_bgd_block(void) {
    return (ext2_block_size == 1024) ? 2 : 1;
}

static int ext2_read_bgd(int group, struct ext2_bgd* bgd) {
    uint64_t offset = (uint64_t)ext2_bgd_block() * ext2_block_size
                    + group * sizeof(struct ext2_bgd);
    return disk_read(offset, bgd, sizeof(struct ext2_bgd));
}

static int ext2_write_block(uint32_t block_num, const void* buf) {
    uint64_t offset = (uint64_t)block_num * ext2_block_size;
    return disk_write(offset, buf, ext2_block_size);
}

static uint32_t ext2_alloc_block(void) {
    uint32_t num_groups = (ext2_sb.s_blocks_count + ext2_blocks_per_group - 1)
                        / ext2_blocks_per_group;

    for (uint32_t g = 0; g < num_groups; g++) {
        struct ext2_bgd bgd;
        if (!ext2_read_bgd(g, &bgd)) continue;
        if (bgd.bg_free_blocks_count == 0) continue;

        static uint8_t bmp[4096];
        if (!ext2_read_block(bgd.bg_block_bitmap, bmp)) continue;

        for (uint32_t i = 0; i < ext2_block_size; i++) {
            if (bmp[i] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (bmp[i] & (1 << bit)) continue;

                bmp[i] |= (1 << bit);
                ext2_write_block(bgd.bg_block_bitmap, bmp);

                bgd.bg_free_blocks_count--;
                disk_write_bgd(g, &bgd);
                ext2_sb.s_free_blocks_count--;

                uint32_t block_num = g * ext2_blocks_per_group + i * 8 + bit;
                static uint8_t zero_buf[4096];
                memset(zero_buf, 0, ext2_block_size);
                ext2_write_block(block_num, zero_buf);
                return block_num;
            }
        }
    }
    return 0;
}

static void ext2_free_block(uint32_t block_num) {
    if (block_num == 0) return;
    uint32_t group = block_num / ext2_blocks_per_group;
    uint32_t index = block_num % ext2_blocks_per_group;

    struct ext2_bgd bgd;
    if (!ext2_read_bgd(group, &bgd)) return;

    static uint8_t bmp[4096];
    if (!ext2_read_block(bgd.bg_block_bitmap, bmp)) return;

    uint32_t byte_idx = index / 8;
    uint32_t bit_idx = index % 8;
    if (byte_idx >= ext2_block_size) return;

    bmp[byte_idx] &= ~(1u << bit_idx);
    ext2_write_block(bgd.bg_block_bitmap, bmp);

    bgd.bg_free_blocks_count++;
    disk_write_bgd(group, &bgd);
    ext2_sb.s_free_blocks_count++;
}

static uint32_t ext2_alloc_inode(void) {
    uint32_t num_groups = (ext2_sb.s_inodes_count + ext2_inodes_per_group - 1)
                        / ext2_inodes_per_group;

    for (uint32_t g = 0; g < num_groups; g++) {
        struct ext2_bgd bgd;
        if (!ext2_read_bgd(g, &bgd)) continue;
        if (bgd.bg_free_inodes_count == 0) continue;

        static uint8_t bmp[4096];
        if (!ext2_read_block(bgd.bg_inode_bitmap, bmp)) continue;

        for (uint32_t i = 0; i < ext2_block_size; i++) {
            if (bmp[i] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (bmp[i] & (1 << bit)) continue;

                bmp[i] |= (1 << bit);
                ext2_write_block(bgd.bg_inode_bitmap, bmp);

                bgd.bg_free_inodes_count--;
                disk_write_bgd(g, &bgd);
                ext2_sb.s_free_inodes_count--;

                return g * ext2_inodes_per_group + i * 8 + bit + 1;
            }
        }
    }
    return 0;
}

static void ext2_free_inode(uint32_t inode_num) {
    if (inode_num == 0) return;
    uint32_t group = (inode_num - 1) / ext2_inodes_per_group;
    uint32_t index = (inode_num - 1) % ext2_inodes_per_group;

    struct ext2_bgd bgd;
    if (!ext2_read_bgd(group, &bgd)) return;

    static uint8_t bmp[4096];
    if (!ext2_read_block(bgd.bg_inode_bitmap, bmp)) return;

    uint32_t byte_idx = index / 8;
    uint32_t bit_idx = index % 8;
    if (byte_idx >= ext2_block_size) return;

    bmp[byte_idx] &= ~(1u << bit_idx);
    ext2_write_block(bgd.bg_inode_bitmap, bmp);

    bgd.bg_free_inodes_count++;
    disk_write_bgd(group, &bgd);
    ext2_sb.s_free_inodes_count++;
}

static int ext2_write_inode(uint32_t inode_num, const struct ext2_inode* ino) {
    if (!ext2_mounted || inode_num == 0) return 0;

    uint32_t group = (inode_num - 1) / ext2_inodes_per_group;
    uint32_t index = (inode_num - 1) % ext2_inodes_per_group;

    struct ext2_bgd bgd;
    if (!ext2_read_bgd(group, &bgd)) return 0;

    uint64_t inode_offset = (uint64_t)bgd.bg_inode_table * ext2_block_size
                          + index * ext2_inode_size;

    static uint8_t sector_buf[512];
    uint64_t sector = inode_offset / 512;
    uint32_t intra = (uint32_t)(inode_offset % 512);
    if (!ahci_read_sectors(sector, 1, sector_buf)) return 0;
    memcpy(sector_buf + intra, ino, ext2_inode_size);
    return ahci_write_sectors(sector, 1, sector_buf) > 0;
}

static void ext2_free_inode_blocks(struct ext2_inode* ino) {
    uint32_t bpb = ext2_block_size / 4;

    for (int i = 0; i < 12; i++) {
        if (ino->i_block[i]) {
            ext2_free_block(ino->i_block[i]);
            ino->i_block[i] = 0;
        }
    }

    if (ino->i_block[12]) {
        uint32_t ptr[1024];
        if (ext2_read_block(ino->i_block[12], ptr)) {
            for (uint32_t i = 0; i < bpb; i++) {
                if (ptr[i]) ext2_free_block(ptr[i]);
            }
        }
        ext2_free_block(ino->i_block[12]);
        ino->i_block[12] = 0;
    }

    if (ino->i_block[13]) {
        uint32_t l1[1024];
        if (ext2_read_block(ino->i_block[13], l1)) {
            for (uint32_t i = 0; i < bpb; i++) {
                if (l1[i]) {
                    uint32_t l2[1024];
                    if (ext2_read_block(l1[i], l2)) {
                        for (uint32_t j = 0; j < bpb; j++) {
                            if (l2[j]) ext2_free_block(l2[j]);
                        }
                    }
                    ext2_free_block(l1[i]);
                }
            }
        }
        ext2_free_block(ino->i_block[13]);
        ino->i_block[13] = 0;
    }

    ino->i_blocks = 0;
}

static int ext2_add_dir_entry(uint32_t dir_inode_num, uint32_t child_inode,
                              const char* name, uint8_t type) {
    struct ext2_inode dir_inode;
    if (!ext2_read_inode(dir_inode_num, &dir_inode)) return 0;

    static uint8_t block_buf[4096];

    for (int i = 0; i < 12; i++) {
        if (dir_inode.i_block[i] == 0) continue;
        if (!ext2_read_block(dir_inode.i_block[i], block_buf)) continue;

        uint32_t offset = 0;
        struct ext2_dirent* last_de = NULL;
        while (offset < ext2_block_size) {
            struct ext2_dirent* de = (struct ext2_dirent*)&block_buf[offset];
            if (de->rec_len == 0) break;
            last_de = de;
            offset += de->rec_len;
        }

        if (last_de && offset < ext2_block_size) {
            uint32_t actual_len = sizeof(struct ext2_dirent) + last_de->name_len;
            actual_len = (actual_len + 3) & ~3u;
            uint32_t remaining = ext2_block_size - offset + last_de->rec_len - actual_len;
            uint32_t needed = sizeof(struct ext2_dirent) + strlen(name);
            needed = (needed + 3) & ~3u;
            if (remaining >= needed) {
                last_de->rec_len = actual_len;
                struct ext2_dirent* new_de = (struct ext2_dirent*)
                    ((uint8_t*)last_de + actual_len);
                new_de->inode = child_inode;
                new_de->name_len = strlen(name);
                new_de->file_type = type;
                memcpy(new_de->name, name, new_de->name_len);
                new_de->rec_len = ext2_block_size - ((uint8_t*)new_de - block_buf);
                ext2_write_block(dir_inode.i_block[i], block_buf);
                return 1;
            }
        }
    }

    for (int i = 0; i < 12; i++) {
        if (dir_inode.i_block[i] == 0) {
            uint32_t new_block = ext2_alloc_block();
            if (!new_block) return 0;

            memset(block_buf, 0, ext2_block_size);
            struct ext2_dirent* de = (struct ext2_dirent*)block_buf;
            de->inode = child_inode;
            de->name_len = strlen(name);
            de->file_type = type;
            memcpy(de->name, name, de->name_len);
            de->rec_len = ext2_block_size;

            ext2_write_block(new_block, block_buf);
            dir_inode.i_block[i] = new_block;
            dir_inode.i_size += ext2_block_size;
            dir_inode.i_blocks += ext2_block_size / 512;
            ext2_write_inode(dir_inode_num, &dir_inode);
            return 1;
        }
    }

    return 0;
}

static int ext2_remove_dir_entry(uint32_t dir_inode_num, const char* name) {
    struct ext2_inode dir_inode;
    if (!ext2_read_inode(dir_inode_num, &dir_inode)) return 0;

    static uint8_t block_buf[4096];
    uint32_t bytes_walked = 0;
    uint32_t logical_block = 0;

    while (bytes_walked < dir_inode.i_size) {
        uint32_t phys = ext2_get_inode_block(&dir_inode, logical_block);
        if (phys == 0) break;
        if (!ext2_read_block(phys, block_buf)) break;

        uint32_t offset = 0;
        struct ext2_dirent* prev_de = NULL;
        while (offset < ext2_block_size && bytes_walked + offset < dir_inode.i_size) {
            struct ext2_dirent* de = (struct ext2_dirent*)&block_buf[offset];
            if (de->rec_len == 0) break;

            if (de->inode != 0 && de->name_len > 0) {
                int match = 1;
                uint32_t nlen = de->name_len;
                for (uint32_t j = 0; j < nlen; j++) {
                    if (name[j] != de->name[j]) { match = 0; break; }
                }
                if (match && name[nlen] == '\0') {
                    if (prev_de) {
                        prev_de->rec_len += de->rec_len;
                    } else {
                        de->inode = 0;
                    }
                    ext2_write_block(phys, block_buf);
                    return 1;
                }
            }
            prev_de = de;
            offset += de->rec_len;
        }
        bytes_walked += ext2_block_size;
        logical_block++;
    }
    return 0;
}

static void ext2_split_path(const char* path, char* parent, char* name) {
    const char* last_slash = path;
    const char* end = path;
    while (*end) { if (*end == '/') last_slash = end; end++; }

    int name_len = (int)(end - last_slash - 1);
    if (name_len < 0) name_len = 0;
    if (name_len > 255) name_len = 255;
    memcpy(name, last_slash + 1, name_len);
    name[name_len] = '\0';

    int parent_len = (int)(last_slash - path);
    if (parent_len == 0) parent_len = 1;
    if (parent_len > 255) parent_len = 255;
    memcpy(parent, path, parent_len);
    parent[parent_len] = '\0';
}

uint32_t ext2_create_file(const char* path, uint16_t mode) {
    if (!ext2_mounted || !path || path[0] != '/') return 0;

    char parent[256], name[256];
    ext2_split_path(path, parent, name);
    if (name[0] == '\0') return 0;

    uint32_t parent_inode_num = ext2_resolve_path(parent);
    if (!parent_inode_num) {
        pr_warn("ext2: parent '%s' not found\n", parent);
        return 0;
    }

    uint32_t new_inode = ext2_alloc_inode();
    if (!new_inode) {
        pr_warn("ext2: no free inodes\n");
        return 0;
    }

    struct ext2_inode ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_mode = mode ? mode : 0x81A4;
    ino.i_atime = ino.i_ctime = ino.i_mtime = (uint32_t)(timer_get_ms() / 1000);
    ino.i_links_count = 1;

    if (!ext2_write_inode(new_inode, &ino)) return 0;

    if (!ext2_add_dir_entry(parent_inode_num, new_inode, name, 1)) return 0;

    pr_info("ext2: created '%s' (inode %u)\n", path, new_inode);
    return new_inode;
}

uint32_t ext2_mkdir(const char* path, uint16_t mode) {
    if (!ext2_mounted || !path || path[0] != '/') return 0;

    char parent[256], name[256];
    ext2_split_path(path, parent, name);
    if (name[0] == '\0') return 0;

    uint32_t parent_inode_num = ext2_resolve_path(parent);
    if (!parent_inode_num) {
        pr_warn("ext2: parent '%s' not found\n", parent);
        return 0;
    }

    uint32_t new_inode = ext2_alloc_inode();
    if (!new_inode) {
        pr_warn("ext2: no free inodes\n");
        return 0;
    }

    uint32_t dir_block = ext2_alloc_block();
    if (!dir_block) return 0;

    static uint8_t block_buf[4096];
    memset(block_buf, 0, ext2_block_size);

    struct ext2_dirent* de = (struct ext2_dirent*)block_buf;
    de->inode = new_inode;
    de->name_len = 1;
    de->file_type = 2;
    de->name[0] = '.';
    uint32_t rec1 = (sizeof(struct ext2_dirent) + 1 + 3) & ~3u;
    de->rec_len = rec1;

    struct ext2_dirent* de2 = (struct ext2_dirent*)(block_buf + rec1);
    de2->inode = parent_inode_num;
    de2->name_len = 2;
    de2->file_type = 2;
    de2->name[0] = '.';
    de2->name[1] = '.';
    de2->rec_len = ext2_block_size - rec1;

    ext2_write_block(dir_block, block_buf);

    struct ext2_inode ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_mode = mode ? mode : 0x41ED;
    ino.i_size = ext2_block_size;
    ino.i_blocks = ext2_block_size / 512;
    ino.i_block[0] = dir_block;
    ino.i_atime = ino.i_ctime = ino.i_mtime = (uint32_t)(timer_get_ms() / 1000);
    ino.i_links_count = 2;

    if (!ext2_write_inode(new_inode, &ino)) return 0;

    struct ext2_inode parent_ino;
    if (ext2_read_inode(parent_inode_num, &parent_ino)) {
        parent_ino.i_links_count++;
        ext2_write_inode(parent_inode_num, &parent_ino);
    }

    if (!ext2_add_dir_entry(parent_inode_num, new_inode, name, 2)) return 0;

    pr_info("ext2: created directory '%s' (inode %u)\n", path, new_inode);
    return new_inode;
}

int ext2_unlink(const char* path) {
    if (!ext2_mounted || !path || path[0] != '/') return 0;

    uint32_t inode_num = ext2_resolve_path(path);
    if (!inode_num) return 0;

    struct ext2_inode ino;
    if (!ext2_read_inode(inode_num, &ino)) return 0;

    char parent[256], name[256];
    ext2_split_path(path, parent, name);

    uint32_t parent_inode_num = ext2_resolve_path(parent);
    if (parent_inode_num) {
        ext2_remove_dir_entry(parent_inode_num, name);
    }

    if ((ino.i_mode & 0xF000) == 0x4000) {
        if (parent_inode_num) {
            struct ext2_inode pin;
            if (ext2_read_inode(parent_inode_num, &pin)) {
                if (pin.i_links_count > 1) pin.i_links_count--;
                ext2_write_inode(parent_inode_num, &pin);
            }
        }
    }

    ext2_free_inode_blocks(&ino);

    ino.i_dtime = (uint32_t)(timer_get_ms() / 1000);
    ino.i_links_count = 0;
    ext2_write_inode(inode_num, &ino);
    ext2_free_inode(inode_num);

    pr_info("ext2: unlinked '%s' (inode %u)\n", path, inode_num);
    return 1;
}

int ext2_write_file(const char* path, const void* buf, uint32_t len) {
    if (!ext2_mounted || !path || !buf || len == 0) return 0;

    uint32_t inode_num = ext2_resolve_path(path);

    if (!inode_num) {
        inode_num = ext2_create_file(path, 0);
        if (!inode_num) return 0;
    }

    struct ext2_inode ino;
    if (!ext2_read_inode(inode_num, &ino)) return 0;

    uint32_t old_blocks = (ino.i_size + ext2_block_size - 1) / ext2_block_size;
    uint32_t new_blocks = (len + ext2_block_size - 1) / ext2_block_size;
    uint32_t bpb = ext2_block_size / 4;

    if (new_blocks < old_blocks) {
        for (uint32_t b = new_blocks; b < old_blocks && b < 12; b++) {
            if (ino.i_block[b]) { ext2_free_block(ino.i_block[b]); ino.i_block[b] = 0; }
        }
        if (new_blocks < 12 && old_blocks > 12 && ino.i_block[12]) {
            uint32_t ptr[1024];
            if (ext2_read_block(ino.i_block[12], ptr)) {
                uint32_t start = (new_blocks > 12) ? new_blocks - 12 : 0;
                for (uint32_t b = start; b < bpb; b++) {
                    if (ptr[b]) { ext2_free_block(ptr[b]); ptr[b] = 0; }
                }
                ext2_write_block(ino.i_block[12], ptr);
            }
            if (new_blocks <= 12) {
                ext2_free_block(ino.i_block[12]);
                ino.i_block[12] = 0;
            }
        }
        if (new_blocks < 12 && old_blocks > 12 + bpb && ino.i_block[13]) {
            uint32_t l1[1024];
            if (ext2_read_block(ino.i_block[13], l1)) {
                for (uint32_t i = 0; i < bpb; i++) {
                    if (l1[i]) {
                        uint32_t l2[1024];
                        if (ext2_read_block(l1[i], l2)) {
                            for (uint32_t j = 0; j < bpb; j++) {
                                if (l2[j]) ext2_free_block(l2[j]);
                            }
                        }
                        ext2_free_block(l1[i]);
                    }
                }
            }
            ext2_free_block(ino.i_block[13]);
            ino.i_block[13] = 0;
        }
    }

    uint32_t bytes_written = 0;
    static uint8_t block_buf[4096];

    while (bytes_written < len) {
        uint32_t block_idx = bytes_written / ext2_block_size;
        uint32_t block_num = 0;

        if (block_idx < 12) {
            block_num = ino.i_block[block_idx];
            if (block_num == 0) {
                block_num = ext2_alloc_block();
                if (!block_num) break;
                ino.i_block[block_idx] = block_num;
                ino.i_blocks += ext2_block_size / 512;
            }
        } else if (block_idx < 12 + bpb) {
            if (ino.i_block[12] == 0) {
                ino.i_block[12] = ext2_alloc_block();
                if (!ino.i_block[12]) break;
                ino.i_blocks += ext2_block_size / 512;
                memset(block_buf, 0, ext2_block_size);
                ext2_write_block(ino.i_block[12], block_buf);
            }
            uint32_t ptr[1024];
            if (!ext2_read_block(ino.i_block[12], ptr)) break;
            uint32_t idx = block_idx - 12;
            block_num = ptr[idx];
            if (block_num == 0) {
                block_num = ext2_alloc_block();
                if (!block_num) break;
                ptr[idx] = block_num;
                ext2_write_block(ino.i_block[12], ptr);
                ino.i_blocks += ext2_block_size / 512;
            }
        } else if (block_idx < 12 + bpb + bpb * bpb) {
            if (ino.i_block[13] == 0) {
                ino.i_block[13] = ext2_alloc_block();
                if (!ino.i_block[13]) break;
                ino.i_blocks += ext2_block_size / 512;
                memset(block_buf, 0, ext2_block_size);
                ext2_write_block(ino.i_block[13], block_buf);
            }
            uint32_t l1[1024];
            if (!ext2_read_block(ino.i_block[13], l1)) break;
            uint32_t rem = block_idx - 12 - bpb;
            uint32_t idx1 = rem / bpb;
            uint32_t idx2 = rem % bpb;
            if (l1[idx1] == 0) {
                l1[idx1] = ext2_alloc_block();
                if (!l1[idx1]) break;
                ext2_write_block(ino.i_block[13], l1);
                ino.i_blocks += ext2_block_size / 512;
                memset(block_buf, 0, ext2_block_size);
                ext2_write_block(l1[idx1], block_buf);
            }
            uint32_t l2[1024];
            if (!ext2_read_block(l1[idx1], l2)) break;
            block_num = l2[idx2];
            if (block_num == 0) {
                block_num = ext2_alloc_block();
                if (!block_num) break;
                l2[idx2] = block_num;
                ext2_write_block(l1[idx1], l2);
                ino.i_blocks += ext2_block_size / 512;
            }
        } else {
            break;
        }

        ext2_read_block(block_num, block_buf);
        uint32_t block_offset = bytes_written % ext2_block_size;
        uint32_t to_write = ext2_block_size - block_offset;
        if (to_write > len - bytes_written) to_write = len - bytes_written;
        memcpy(block_buf + block_offset, (const uint8_t*)buf + bytes_written, to_write);
        ext2_write_block(block_num, block_buf);
        bytes_written += to_write;
    }

    ino.i_size = bytes_written;
    ino.i_mtime = (uint32_t)(timer_get_ms() / 1000);
    ext2_write_inode(inode_num, &ino);

    pr_info("ext2: wrote %u bytes to '%s' (inode %u)\n", bytes_written, path, inode_num);
    return (int)bytes_written;
}

int ext2_is_writable(void) {
    return ext2_mounted;
}
