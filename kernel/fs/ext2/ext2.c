/*
 * Lestra OS - ext2 read-only filesystem
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * A minimal read-only ext2 filesystem driver. It can:
 *   - Mount an ext2 filesystem on a block device (AHCI SATA drive)
 *   - Read the root directory
 *   - Look up files by path
 *   - Read file contents
 *
 * Limitations:
 *   - Read-only (no writes)
 *   - No directory nesting beyond root + one level (for simplicity)
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

/* External: AHCI sector read */
extern int ahci_read_sectors(uint64_t lba, uint32_t count, void* buf);
extern int ahci_has_drive(void);

/* ext2 superblock (located at offset 1024, 1024 bytes) */
struct ext2_superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;   /* block size = 1024 << s_log_block_size */
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;            /* 0xEF53 */
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* EXT2_DYNAMIC_REV specific fields */
    uint32_t s_first_ino;
    uint16_t s_inode_size;       /* Usually 128 or 256 */
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    /* ... more fields we don't need ... */
} __packed;

/* Block group descriptor (32 bytes) */
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

/* Inode (128 bytes for rev 0, up to 256 for dynamic) */
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
    uint32_t i_block[15];   /* 12 direct + 1 indirect + 1 double + 1 triple */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __packed;

/* Directory entry (variable length) */
struct ext2_dirent {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} __packed;

/* ext2 state */
static int ext2_mounted = 0;
static uint32_t ext2_block_size = 1024;
static uint32_t ext2_inodes_per_group;
static uint32_t ext2_blocks_per_group;
static uint16_t ext2_inode_size;
static struct ext2_superblock ext2_sb;

/* Sector read helper: reads `count` bytes at byte offset `offset` from disk.
 * Assumes 512-byte sectors. */
static int disk_read(uint64_t offset, void* buf, uint32_t count) {
    if (!ahci_has_drive()) return 0;
    uint64_t lba = offset / 512;
    uint32_t sectors = (count + 511) / 512;
    if (sectors > 8) sectors = 8;  /* max 8 sectors per read */
    return ahci_read_sectors(lba, sectors, buf) > 0;
}

/* Sector write helper */
static int disk_write(uint64_t offset, const void* buf, uint32_t count) {
    if (!ahci_has_drive()) return 0;
    extern int ahci_write_sectors(uint64_t lba, uint32_t count, const void* buf);
    uint64_t lba = offset / 512;
    uint32_t sectors = (count + 511) / 512;
    if (sectors > 8) sectors = 8;
    return ahci_write_sectors(lba, sectors, buf) > 0;
}

/* Write a block group descriptor back to disk */
static int disk_write_bgd(int group, const struct ext2_bgd* bgd) {
    uint64_t offset;
    if (ext2_block_size == 1024) {
        offset = 2 * ext2_block_size + group * sizeof(struct ext2_bgd);
    } else {
        offset = 1 * ext2_block_size + group * sizeof(struct ext2_bgd);
    }
    return disk_write(offset, bgd, sizeof(struct ext2_bgd));
}

/* Read an ext2 block (block_num is 1-based). */
static int ext2_read_block(uint32_t block_num, void* buf) {
    uint64_t offset = (uint64_t)block_num * ext2_block_size;
    return disk_read(offset, buf, ext2_block_size);
}

int ext2_mount(void) {
    if (!ahci_has_drive()) {
        pr_info("ext2: no disk drive available\n");
        return 0;
    }

    /* Read the superblock (at offset 1024, size 1024) */
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

/* Read an inode by number. Returns 1 on success. */
static int ext2_read_inode(uint32_t inode_num, struct ext2_inode* out) {
    if (!ext2_mounted || inode_num == 0) return 0;

    /* Inode 1 is in group 0, index 0. Inode N is in group (N-1)/inodes_per_group,
     * index (N-1) % inodes_per_group. */
    uint32_t group = (inode_num - 1) / ext2_inodes_per_group;
    uint32_t index = (inode_num - 1) % ext2_inodes_per_group;

    /* Read the block group descriptor (in the block after the superblock).
     * If block_size == 1024, superblock is in block 1, BGDs start in block 2.
     * If block_size > 1024, superblock is in block 0, BGDs start in block 1. */
    uint32_t bgd_block;
    if (ext2_block_size == 1024) {
        bgd_block = 2;
    } else {
        bgd_block = 1;
    }
    uint32_t bgd_offset = bgd_block * ext2_block_size + group * sizeof(struct ext2_bgd);

    static uint8_t block_buf[4096];
    /* Read the block containing this BGD */
    if (!disk_read(bgd_block * ext2_block_size, block_buf, ext2_block_size)) {
        return 0;
    }

    struct ext2_bgd* bgd = (struct ext2_bgd*)&block_buf[group * sizeof(struct ext2_bgd)];
    uint32_t inode_table_block = bgd->bg_inode_table;

    /* Read the inode from the inode table */
    uint64_t inode_offset = (uint64_t)inode_table_block * ext2_block_size
                          + index * ext2_inode_size;
    static uint8_t inode_buf[256];
    if (!disk_read(inode_offset, inode_buf, ext2_inode_size)) {
        return 0;
    }

    memcpy(out, inode_buf, sizeof(struct ext2_inode));
    return 1;
}

/* Find a directory entry by name in the directory identified by `dir_inode`.
 * Returns the inode number of the entry, or 0 if not found. */
static uint32_t ext2_find_dirent(struct ext2_inode* dir_inode, const char* name) {
    if (!ext2_mounted) return 0;

    /* Read directory data from direct blocks (i_block[0..11]) */
    static uint8_t block_buf[4096];
    for (int i = 0; i < 12; i++) {
        if (dir_inode->i_block[i] == 0) continue;

        if (!ext2_read_block(dir_inode->i_block[i], block_buf)) continue;

        /* Parse directory entries */
        uint32_t offset = 0;
        while (offset < ext2_block_size && offset < dir_inode->i_size) {
            struct ext2_dirent* de = (struct ext2_dirent*)&block_buf[offset];
            if (de->rec_len == 0) break;

            if (de->inode != 0 && de->name_len > 0) {
                /* Compare name (name_len chars, not null-terminated) */
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
    }
    return 0;
}

/* Resolve a path like "/foo/bar" to an inode number.
 * Root directory is inode 2 in ext2. */
static uint32_t ext2_resolve_path(const char* path) {
    if (!ext2_mounted || !path || path[0] != '/') return 0;

    uint32_t inode_num = 2;  /* root directory */
    struct ext2_inode inode;

    if (!ext2_read_inode(inode_num, &inode)) return 0;

    /* If path is just "/", return root */
    if (path[1] == '\0') return inode_num;

    /* Walk path components */
    const char* p = path + 1;  /* skip leading '/' */
    while (*p) {
        /* Extract component name */
        char name[256];
        int len = 0;
        while (*p && *p != '/' && len < 255) {
            name[len++] = *p++;
        }
        name[len] = '\0';
        if (*p == '/') p++;

        if (len == 0) continue;

        /* Find this name in the current directory inode */
        inode_num = ext2_find_dirent(&inode, name);
        if (inode_num == 0) return 0;  /* not found */

        /* Read the next inode */
        if (!ext2_read_inode(inode_num, &inode)) return 0;
    }

    return inode_num;
}

/* Read file contents into buf (up to bufsize bytes). Returns bytes read. */
int ext2_read_file(const char* path, void* buf, uint32_t bufsize) {
    if (!ext2_mounted) return 0;

    uint32_t inode_num = ext2_resolve_path(path);
    if (inode_num == 0) {
        pr_warn("ext2: file '%s' not found\n", path);
        return 0;
    }

    struct ext2_inode inode;
    if (!ext2_read_inode(inode_num, &inode)) return 0;

    /* Check it's a regular file (mode bits 0x8000) */
    if ((inode.i_mode & 0xF000) != 0x8000) {
        pr_warn("ext2: '%s' is not a regular file\n", path);
        return 0;
    }

    uint32_t file_size = inode.i_size;
    if (file_size > bufsize) file_size = bufsize;

    /* Read from direct blocks (i_block[0..11]) */
    static uint8_t block_buf[4096];
    uint32_t bytes_read = 0;
    for (int i = 0; i < 12 && bytes_read < file_size; i++) {
        if (inode.i_block[i] == 0) break;

        if (!ext2_read_block(inode.i_block[i], block_buf)) break;

        uint32_t to_copy = ext2_block_size;
        if (to_copy > file_size - bytes_read) to_copy = file_size - bytes_read;
        memcpy((uint8_t*)buf + bytes_read, block_buf, to_copy);
        bytes_read += to_copy;
    }

    return (int)bytes_read;
}

/* List the root directory. Calls callback for each entry. */
void ext2_list_root(void (*callback)(const char* name, uint32_t inode, uint8_t type)) {
    if (!ext2_mounted) return;

    struct ext2_inode inode;
    if (!ext2_read_inode(2, &inode)) return;  /* root = inode 2 */

    static uint8_t block_buf[4096];
    for (int i = 0; i < 12; i++) {
        if (inode.i_block[i] == 0) continue;
        if (!ext2_read_block(inode.i_block[i], block_buf)) continue;

        uint32_t offset = 0;
        while (offset < ext2_block_size && offset < inode.i_size) {
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
    }
}

/* ========================================================================
 * WRITE SUPPORT
 * ======================================================================== */

/* AHCI sector write — we need to add this to the AHCI driver.
 * For now, declare it extern; the driver will be updated. */
extern int ahci_write_sectors(uint64_t lba, uint32_t count, const void* buf);

/* Write a block to disk */
static int ext2_write_block(uint32_t block_num, const void* buf) {
    uint64_t offset = (uint64_t)block_num * ext2_block_size;
    uint64_t lba = offset / 512;
    uint32_t sectors = ext2_block_size / 512;
    return ahci_write_sectors(lba, sectors, buf) > 0;
}

/* Allocate a free block from the block bitmap of group `group`.
 * Returns the block number, or 0 if no free blocks. */
static uint32_t ext2_alloc_block(int group) {
    /* Read the block bitmap for this group */
    struct ext2_bgd bgd;
    static uint8_t block_buf[4096];
    if (!disk_read(2 * ext2_block_size + group * sizeof(struct ext2_bgd),
                   &bgd, sizeof(bgd))) return 0;

    uint32_t bitmap_block = bgd.bg_block_bitmap;
    if (!ext2_read_block(bitmap_block, block_buf)) return 0;

    /* Scan for a free bit (0 = free) */
    for (uint32_t i = 0; i < ext2_block_size; i++) {
        if (block_buf[i] != 0xFF) {
            /* Find the first 0 bit */
            for (int bit = 0; bit < 8; bit++) {
                if (!(block_buf[i] & (1 << bit))) {
                    /* Found a free block */
                    block_buf[i] |= (1 << bit);
                    ext2_write_block(bitmap_block, block_buf);

                    /* Update free blocks count */
                    bgd.bg_free_blocks_count--;
                    disk_write_bgd(group, &bgd);
                    ext2_sb.s_free_blocks_count--;

                    /* Compute the actual block number */
                    uint32_t block_num = group * ext2_blocks_per_group + i * 8 + bit;
                    /* Zero the new block */
                    memset(block_buf, 0, ext2_block_size);
                    ext2_write_block(block_num, block_buf);
                    return block_num;
                }
            }
        }
    }
    return 0;  /* no free blocks in this group */
}

/* Allocate a free inode from the inode bitmap of group `group`.
 * Returns the inode number, or 0 if no free inodes. */
static uint32_t ext2_alloc_inode(int group) {
    struct ext2_bgd bgd;
    static uint8_t block_buf[4096];
    if (!disk_read(2 * ext2_block_size + group * sizeof(struct ext2_bgd),
                   &bgd, sizeof(bgd))) return 0;

    uint32_t bitmap_block = bgd.bg_inode_bitmap;
    if (!ext2_read_block(bitmap_block, block_buf)) return 0;

    for (uint32_t i = 0; i < ext2_block_size; i++) {
        if (block_buf[i] != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                if (!(block_buf[i] & (1 << bit))) {
                    block_buf[i] |= (1 << bit);
                    ext2_write_block(bitmap_block, block_buf);
                    bgd.bg_free_inodes_count--;
                    disk_write_bgd(group, &bgd);
                    ext2_sb.s_free_inodes_count--;
                    uint32_t inode_num = group * ext2_inodes_per_group + i * 8 + bit + 1;
                    return inode_num;
                }
            }
        }
    }
    return 0;
}

/* Write an inode back to disk */
static int ext2_write_inode(uint32_t inode_num, const struct ext2_inode* ino) {
    if (!ext2_mounted || inode_num == 0) return 0;

    uint32_t group = (inode_num - 1) / ext2_inodes_per_group;
    uint32_t index = (inode_num - 1) % ext2_inodes_per_group;

    struct ext2_bgd bgd;
    if (!disk_read(2 * ext2_block_size + group * sizeof(struct ext2_bgd),
                   &bgd, sizeof(bgd))) return 0;

    uint64_t inode_offset = (uint64_t)bgd.bg_inode_table * ext2_block_size
                          + index * ext2_inode_size;
    return ahci_write_sectors(inode_offset / 512, 1, ino) > 0;
}

/* Create a new file in the root directory.
 * Returns the inode number, or 0 on failure. */
uint32_t ext2_create_file(const char* name) {
    if (!ext2_mounted || !name) return 0;

    /* Allocate a new inode in group 0 */
    uint32_t new_inode = ext2_alloc_inode(0);
    if (!new_inode) {
        pr_warn("ext2: no free inodes\n");
        return 0;
    }

    /* Initialize the inode as a regular file (mode 0100644) */
    struct ext2_inode ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_mode = 0x81A4;  /* regular file, rw-r--r-- */
    ino.i_uid = 0;
    ino.i_size = 0;
    ino.i_atime = ino.i_ctime = ino.i_mtime = (uint32_t)timer_get_ms() / 1000;
    ino.i_dtime = 0;
    ino.i_gid = 0;
    ino.i_links_count = 1;
    ino.i_blocks = 0;
    ino.i_flags = 0;

    if (!ext2_write_inode(new_inode, &ino)) {
        pr_warn("ext2: failed to write new inode %u\n", new_inode);
        return 0;
    }

    /* Add a directory entry to the root directory (inode 2) */
    struct ext2_inode root_inode;
    if (!ext2_read_inode(2, &root_inode)) return 0;

    /* Find a block in the root directory with space, or allocate a new one */
    static uint8_t block_buf[4096];
    int block_idx = -1;
    for (int i = 0; i < 12; i++) {
        if (root_inode.i_block[i] == 0) continue;
        if (!ext2_read_block(root_inode.i_block[i], block_buf)) continue;

        /* Check if there's space at the end of this block */
        uint32_t offset = 0;
        struct ext2_dirent* last_de = NULL;
        while (offset < ext2_block_size) {
            struct ext2_dirent* de = (struct ext2_dirent*)&block_buf[offset];
            if (de->rec_len == 0) break;
            last_de = de;
            offset += de->rec_len;
        }

        if (last_de && offset < ext2_block_size) {
            /* There's space — shrink the last entry and add ours */
            uint32_t actual_len = sizeof(struct ext2_dirent) + last_de->name_len;
            actual_len = (actual_len + 3) & ~3u;  /* align to 4 */
            uint32_t remaining = ext2_block_size - offset + last_de->rec_len - actual_len;
            if (remaining >= sizeof(struct ext2_dirent) + strlen(name) + 3) {
                last_de->rec_len = actual_len;
                struct ext2_dirent* new_de = (struct ext2_dirent*)
                    ((uint8_t*)last_de + actual_len);
                new_de->inode = new_inode;
                new_de->name_len = strlen(name);
                new_de->file_type = 1;  /* regular file */
                memcpy(new_de->name, name, new_de->name_len);
                new_de->rec_len = ext2_block_size - ((uint8_t*)new_de - block_buf);

                ext2_write_block(root_inode.i_block[i], block_buf);
                pr_info("ext2: created file '%s' (inode %u)\n", name, new_inode);
                return new_inode;
            }
        }
    }

    /* No space in existing blocks — allocate a new block for root dir */
    for (int i = 0; i < 12; i++) {
        if (root_inode.i_block[i] == 0) {
            uint32_t new_block = ext2_alloc_block(0);
            if (!new_block) return 0;

            memset(block_buf, 0, ext2_block_size);
            struct ext2_dirent* de = (struct ext2_dirent*)block_buf;
            de->inode = new_inode;
            de->name_len = strlen(name);
            de->file_type = 1;
            memcpy(de->name, name, de->name_len);
            de->rec_len = ext2_block_size;

            ext2_write_block(new_block, block_buf);
            root_inode.i_block[i] = new_block;
            root_inode.i_size += ext2_block_size;
            root_inode.i_blocks += ext2_block_size / 512;
            ext2_write_inode(2, &root_inode);

            pr_info("ext2: created file '%s' (inode %u, new block %u)\n",
                    name, new_inode, new_block);
            return new_inode;
        }
    }

    pr_warn("ext2: root directory full (no direct blocks left)\n");
    return 0;
}

/* Write data to a file (identified by path). Creates the file if it
 * doesn't exist. Returns bytes written, or 0 on failure. */
int ext2_write_file(const char* path, const void* buf, uint32_t len) {
    if (!ext2_mounted || !path || !buf || len == 0) return 0;

    /* Try to resolve the path first */
    uint32_t inode_num = ext2_resolve_path(path);

    /* If file doesn't exist, create it */
    if (!inode_num) {
        /* Extract filename from path (last component after '/') */
        const char* name = path;
        const char* p = path;
        while (*p) {
            if (*p == '/') name = p + 1;
            p++;
        }
        inode_num = ext2_create_file(name);
        if (!inode_num) return 0;
    }

    /* Read the inode */
    struct ext2_inode ino;
    if (!ext2_read_inode(inode_num, &ino)) return 0;

    /* Write data to direct blocks */
    uint32_t bytes_written = 0;
    static uint8_t block_buf[4096];

    while (bytes_written < len) {
        int block_idx = bytes_written / ext2_block_size;
        if (block_idx >= 12) break;  /* only direct blocks for now */

        uint32_t block_num = ino.i_block[block_idx];
        if (block_num == 0) {
            /* Allocate a new block */
            block_num = ext2_alloc_block(0);
            if (!block_num) break;
            ino.i_block[block_idx] = block_num;
            ino.i_blocks += ext2_block_size / 512;
        }

        /* Read existing block (to preserve partial writes) */
        ext2_read_block(block_num, block_buf);

        /* Copy data into the block */
        uint32_t block_offset = bytes_written % ext2_block_size;
        uint32_t to_write = ext2_block_size - block_offset;
        if (to_write > len - bytes_written) to_write = len - bytes_written;
        memcpy(block_buf + block_offset, (const uint8_t*)buf + bytes_written, to_write);

        /* Write the block back */
        ext2_write_block(block_num, block_buf);
        bytes_written += to_write;
    }

    /* Update inode size and timestamps */
    if (bytes_written > ino.i_size) {
        ino.i_size = bytes_written;
    }
    ino.i_mtime = (uint32_t)timer_get_ms() / 1000;
    ext2_write_inode(inode_num, &ino);

    pr_info("ext2: wrote %u bytes to '%s' (inode %u)\n",
            bytes_written, path, inode_num);
    return (int)bytes_written;
}

int ext2_is_writable(void) {
    return ext2_mounted;
}
