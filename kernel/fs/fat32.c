/*
 * Lestra OS - FAT32 Read-Write Filesystem Driver
 * Copyright (c) 2026 lestramk.org
 *
 * KE-24 adds full write support: cluster allocation via free-cluster
 * scanning, FAT entry updates (write-through, dual-FAT mirror), file
 * data writes with cluster chain extension, file creation/deletion,
 * subdirectory creation, and directory entry size updates.
 *
 * Write strategy:
 *   - All writes go through the block device write function.
 *   - FAT updates are written immediately (write-through, no caching).
 *   - Cluster allocation uses linear FAT scan (fine for 16MB images).
 *   - Directory modifications update the on-disk cluster chain immediately.
 *
 * Safety: fat32.img is in git; corruption is a git checkout away.
 */

#include <lestra/fat32.h>
#include <lestra/printk.h>
#include <string.h>

/* Sector I/O buffers */
static uint8_t sec_buf[512];
static uint8_t sec_buf2[512];

/* Block device I/O functions */
static fat32_read_fn  blk_read;
static fat32_write_fn blk_write;

/* Parsed BPB */
static struct fat32_bpb bpb;
static uint8_t num_fats = 1;  /* Number of FAT copies (from BPB offset 16) */

/* Mount state */
static int mounted = 0;

/* ---- Low-level sector I/O ---- */

static int read_sector(uint32_t lba) {
    if (blk_read((uint64_t)lba, 1, sec_buf) < 0) return -1;
    return 0;
}

static int read_sectors(uint32_t lba, uint32_t count, void *buf) {
    if (blk_read((uint64_t)lba, count, buf) < 0) return -1;
    return 0;
}

static int write_sector(uint32_t lba, const void *data) {
    if (!blk_write) return -1;
    if (blk_write((uint64_t)lba, 1, data) < 0) return -1;
    return 0;
}

static int write_sectors(uint32_t lba, uint32_t count, const void *data) {
    if (!blk_write) return -1;
    if (blk_write((uint64_t)lba, count, data) < 0) return -1;
    return 0;
}

/* ---- FAT access ---- */

static uint32_t fat_get(uint32_t cluster) {
    if (cluster < 2) return 0x0FFFFFFF;
    uint32_t fat_byte = bpb.fat_offset + cluster * 4;
    uint32_t fat_sec  = fat_byte / 512;
    uint32_t fat_off  = fat_byte % 512;
    if (read_sector(fat_sec) < 0) return 0x0FFFFFFF;
    uint32_t entry;
    memcpy(&entry, sec_buf + fat_off, 4);
    return entry & 0x0FFFFFFF;
}

/* Set FAT entry. Read-modify-write on the FAT sector.
 * Mirrors to second FAT copy if present. */
static int fat_set(uint32_t cluster, uint32_t value) {
    if (cluster < 2 || !blk_write) return -1;
    uint32_t fat_byte = bpb.fat_offset + cluster * 4;
    uint32_t fat_sec  = fat_byte / 512;
    uint32_t fat_off  = fat_byte % 512;

    if (read_sector(fat_sec) < 0) return -1;
    memcpy(sec_buf + fat_off, &value, 4);
    if (write_sector(fat_sec, sec_buf) < 0) return -1;

    /* Mirror to second FAT copy ONLY if NumFATs >= 2.
     * CRITICAL: when NumFATs=1, there IS no second FAT —
     * mirroring would write FAT data into the data region,
     * corrupting the filesystem! */
    if (num_fats >= 2 && bpb.fat_size_sectors > 0) {
        uint32_t fat2_sec = fat_sec + bpb.fat_size_sectors;
        if (write_sector(fat2_sec, sec_buf) < 0)
            pr_warn("fat32: second FAT write failed at sector %u\n", fat2_sec);
    }
    return 0;
}

/* Allocate a free cluster. Returns cluster number (>= 2) or 0. */
static uint32_t fat32_alloc_cluster(void) {
    if (!blk_write) return 0;
    uint32_t fat_start_sec = bpb.fat_offset / 512;
    uint32_t fat_end_sec = fat_start_sec + bpb.fat_size_sectors;
    uint32_t entries_per_sec = 512 / 4;

    for (uint32_t sec = fat_start_sec; sec < fat_end_sec; sec++) {
        if (read_sector(sec) < 0) continue;
        uint32_t cluster_base = (sec - fat_start_sec) * entries_per_sec + 2;
        for (uint32_t i = 0; i < entries_per_sec; i++) {
            uint32_t cluster = cluster_base + i;
            if (cluster >= 2 + bpb.total_clusters) break;
            uint32_t entry;
            memcpy(&entry, sec_buf + i * 4, 4);
            if ((entry & 0x0FFFFFFF) == 0) {
                if (fat_set(cluster, 0x0FFFFFF8) < 0) return 0;
                return cluster;
            }
        }
    }
    pr_warn("fat32: no free clusters\n");
    return 0;
}

/* Free a cluster chain. */
static void fat32_free_chain(uint32_t cluster) {
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t next = fat_get(cluster);
        fat_set(cluster, 0);
        cluster = next;
    }
}

/* Forward decl: zero_cluster (added for FAT32 write support) is defined
 * above cluster_to_offset. Resolve the implicit-declaration build break. */
static uint64_t cluster_to_offset(uint32_t cluster);

/* Zero-fill a newly allocated cluster on disk. */
static int zero_cluster(uint32_t cluster) {
    uint64_t off = cluster_to_offset(cluster);
    uint32_t sec = (uint32_t)(off / 512);
    uint32_t cnt = bpb.cluster_size / 512;
    memset(sec_buf2, 0, 512);
    for (uint32_t s = 0; s < cnt; s++) {
        if (write_sector(sec + s, sec_buf2) < 0) return -1;
    }
    return 0;
}

static uint64_t cluster_to_offset(uint32_t cluster) {
    return bpb.data_offset + (uint64_t)(cluster - 2) * bpb.cluster_size;
}

/* ---- Directory entry parsing/formatting ---- */

static int parse_dirent(const uint8_t *raw, struct fat32_dirent *out) {
    if (raw[0] == 0x00) return -1;
    if (raw[0] == 0xE5) return -2;
    uint8_t attr = raw[11];
    if (attr == 0x0F) return -3;

    char name[12];
    memcpy(name, raw, 11);
    name[11] = '\0';

    int ni = 0;
    for (int i = 0; i < 8 && name[i] != ' '; i++)
        out->name[ni++] = name[i];
    if (name[8] != ' ') {
        out->name[ni++] = '.';
        for (int i = 8; i < 11 && name[i] != ' '; i++)
            out->name[ni++] = name[i];
    }
    out->name[ni] = '\0';

    out->attr = attr;
    out->first_cluster = ((uint32_t)raw[20] << 16) | ((uint32_t)raw[21] << 24)
                        | (uint32_t)raw[26] | ((uint32_t)raw[27] << 8);
    out->file_size = (uint32_t)raw[28] | ((uint32_t)raw[29] << 8)
                      | ((uint32_t)raw[30] << 16) | ((uint32_t)raw[31] << 24);
    out->is_dir = (attr & 0x10) ? 1 : 0;
    return 0;
}

/* Format 8.3 name ("HELLO.TXT") into 11-byte raw field ("HELLO   TXT"). */
static void format_83_name(const char *name83, uint8_t *out) {
    memset(out, ' ', 11);
    const char *dot = strchr(name83, '.');
    if (dot) {
        int name_len = (int)(dot - name83);
        if (name_len > 8) name_len = 8;
        for (int i = 0; i < name_len; i++)
            out[i] = (uint8_t)name83[i];
        const char *ext = dot + 1;
        int ext_len = (int)strlen(ext);
        if (ext_len > 3) ext_len = 3;
        for (int i = 0; i < ext_len; i++)
            out[8 + i] = (uint8_t)ext[i];
    } else {
        int name_len = (int)strlen(name83);
        if (name_len > 8) name_len = 8;
        for (int i = 0; i < name_len; i++)
            out[i] = (uint8_t)name83[i];
    }
}

/* ---- Directory cluster chain I/O ---- */

static int read_dir_cluster_chain(uint32_t start_cluster, uint8_t *buf,
                                    uint32_t bufsize) {
    uint32_t cluster = start_cluster;
    uint32_t total = 0;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        if (total + bpb.cluster_size > bufsize) break;
        uint64_t off = cluster_to_offset(cluster);
        uint32_t sec = (uint32_t)(off / 512);
        uint32_t cnt = bpb.cluster_size / 512;
        if (read_sectors(sec, cnt, buf + total) < 0) return -1;
        total += bpb.cluster_size;
        cluster = fat_get(cluster);
    }
    return (int)total;
}

static int parse_dir_entries(const uint8_t *buf, int raw_len,
                              struct fat32_dirent *out, int max) {
    int count = 0;
    for (int off = 0; off + 32 <= raw_len && count < max; off += 32) {
        const uint8_t *entry = buf + off;
        if (entry[0] == 0x00) break;
        if (entry[0] == 0xE5) continue;
        uint8_t attr = entry[11];
        if (attr == 0x0F) continue;
        if (attr & 0x08) continue;
        int rc = parse_dirent(entry, &out[count]);
        if (rc == 0) count++;
    }
    return count;
}

/* Write a 32-byte directory entry into a directory's cluster chain.
 * If `replace_name` is non-NULL, finds and replaces the entry with
 * that name. Otherwise, appends a new entry (or uses a deleted slot).
 * Returns 0 on success, -1 on error. */
static int write_dir_entry(uint32_t dir_cluster,
                            const uint8_t *raw_entry,
                            const char *replace_name) {
    if (!blk_write || dir_cluster < 2) return -1;

    /* Read the full directory into a buffer */
    static uint8_t dir_buf[16 * 512 * 4];  /* 32 KB max */
    int raw = read_dir_cluster_chain(dir_cluster, dir_buf, sizeof(dir_buf));
    if (raw < 0) return -1;

    /* Zero-fill any unwritten portion at the end (for new clusters) */
    /* Find the actual end by looking for the 0x00 terminator */
    int end_off = 0;
    for (int off = 0; off < raw; off += 32) {
        if (dir_buf[off] == 0x00) { end_off = off; break; }
        end_off = off + 32;
    }

    if (replace_name) {
        /* Find and replace existing entry */
        uint8_t target[11];
        format_83_name(replace_name, target);
        for (int off = 0; off < end_off; off += 32) {
            if (memcmp(dir_buf + off, target, 11) == 0) {
                /* Preserve the first byte if it's a LFN marker */
                memcpy(dir_buf + off, raw_entry, 32);
                goto write_back;
            }
        }
        pr_warn("fat32: dir entry '%s' not found for update\n", replace_name);
        return -1;
    }

    /* Append: look for a deleted entry (0xE5) or use the end */
    for (int off = 0; off < end_off; off += 32) {
        if (dir_buf[off] == 0xE5) {
            memcpy(dir_buf + off, raw_entry, 32);
            goto write_back;
        }
    }

    /* No deleted slot — append at end_off.
     * Need space: if we're at the cluster boundary, extend the chain. */
    if (end_off + 32 > raw) {
        /* Extend directory cluster chain */
        uint32_t last_cluster = dir_cluster;
        uint32_t clust_off = 0;
        while (1) {
            uint32_t next = fat_get(last_cluster);
            if (next >= 2 && next < 0x0FFFFFF8) {
                last_cluster = next;
                clust_off += bpb.cluster_size;
            } else {
                /* Allocate new cluster */
                uint32_t nc = fat32_alloc_cluster();
                if (nc == 0) return -1;
                if (zero_cluster(nc) < 0) return -1;
                fat_set(last_cluster, nc);
                /* Read the newly extended dir into the buffer */
                raw = read_dir_cluster_chain(dir_cluster, dir_buf, sizeof(dir_buf));
                if (raw < 0) return -1;
                /* Find the new end (should be right after old data) */
                end_off = clust_off + bpb.cluster_size;
                break;
            }
        }
    }

    memcpy(dir_buf + end_off, raw_entry, 32);

write_back:
    /* Write back the entire directory cluster chain */
    uint32_t cluster = dir_cluster;
    uint32_t buf_off = 0;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint64_t coff = cluster_to_offset(cluster);
        uint32_t sec = (uint32_t)(coff / 512);
        uint32_t cnt = bpb.cluster_size / 512;
        if (write_sectors(sec, cnt, dir_buf + buf_off) < 0) return -1;
        buf_off += bpb.cluster_size;
        cluster = fat_get(cluster);
    }
    return 0;
}

/* ---- Public API: Init ---- */

int fat32_init(fat32_read_fn read_fn) {
    if (!read_fn) return -1;
    blk_read = read_fn;
    blk_write = NULL;

    if (read_sector(0) < 0) {
        pr_warn("fat32: failed to read boot sector\n");
        return -1;
    }

    if (sec_buf[510] != 0x55 || sec_buf[511] != 0xAA) {
        pr_warn("fat32: invalid boot signature\n");
        return -1;
    }

    uint16_t bps;
    memcpy(&bps, sec_buf + 11, 2);
    if (bps != 512) {
        pr_warn("fat32: unsupported sector size %u\n", bps);
        return -1;
    }

    bpb.bytes_per_sector = bps;
    bpb.sectors_per_cluster = sec_buf[13];
    bpb.reserved_sectors = sec_buf[14] | (sec_buf[15] << 8);
    num_fats = sec_buf[16];  /* Number of FAT copies (BPB offset 16) */

    uint32_t fat_size;
    memcpy(&fat_size, sec_buf + 36, 4);
    bpb.fat_offset = bpb.reserved_sectors * 512;
    bpb.fat_size_sectors = fat_size;
    bpb.data_offset = bpb.fat_offset + fat_size * num_fats * 512;
    bpb.cluster_size = bpb.bytes_per_sector * bpb.sectors_per_cluster;

    memcpy(&bpb.root_cluster, sec_buf + 44, 4);

    uint32_t total_sec;
    memcpy(&total_sec, sec_buf + 32, 4);
    if (total_sec == 0) {
        uint16_t ts;
        memcpy(&ts, sec_buf + 19, 2);
        total_sec = ts;
    }
    uint32_t data_sec = total_sec - bpb.reserved_sectors - fat_size * num_fats;
    bpb.total_clusters = data_sec / bpb.sectors_per_cluster;

    mounted = 1;
    pr_info("fat32: mounted r/w (bps=%u, spc=%u, root=%u, clusters=%u, data@0x%x, fat_sec=%u, num_fats=%u)\n",
            bpb.bytes_per_sector, bpb.sectors_per_cluster,
            bpb.root_cluster, bpb.total_clusters, bpb.data_offset,
            bpb.fat_size_sectors, (unsigned)num_fats);
    return 0;
}

void fat32_set_write_fn(fat32_write_fn write_fn) {
    blk_write = write_fn;
    pr_info("fat32: write support %s\n", write_fn ? "enabled" : "disabled");
}

int fat32_is_writable(void) { return mounted && blk_write != NULL; }
int fat32_is_mounted(void) { return mounted; }

const struct fat32_bpb* fat32_get_bpb(void) { return &bpb; }

/* ---- Public API: Read ---- */

int fat32_list_root(struct fat32_dirent *out, int max) {
    if (!mounted) return -1;
    static uint8_t dir_buf[8 * 512 * 4];
    int raw = read_dir_cluster_chain(bpb.root_cluster, dir_buf, sizeof(dir_buf));
    if (raw < 0) return -1;
    return parse_dir_entries(dir_buf, raw, out, max);
}

int fat32_list_dir(uint32_t dir_cluster, struct fat32_dirent *out, int max) {
    if (!mounted || dir_cluster < 2) return -1;
    static uint8_t dir_buf[8 * 512 * 4];
    int raw = read_dir_cluster_chain(dir_cluster, dir_buf, sizeof(dir_buf));
    if (raw < 0) return -1;
    return parse_dir_entries(dir_buf, raw, out, max);
}

int fat32_read_file(uint32_t first_cluster, uint32_t size,
                       void *buf, uint32_t bufsize) {
    if (!mounted || first_cluster < 2) return -1;
    uint32_t to_read = (size < bufsize) ? size : bufsize;
    uint32_t remaining = to_read;
    uint32_t cluster = first_cluster;
    uint32_t offset = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8 && remaining > 0) {
        uint64_t coff = cluster_to_offset(cluster);
        uint32_t sec = (uint32_t)(coff / 512);
        uint32_t cnt = bpb.cluster_size / 512;
        uint32_t chunk = (remaining < bpb.cluster_size) ? remaining : bpb.cluster_size;
        if (read_sectors(sec, cnt, (uint8_t*)buf + offset) < 0) return -1;
        offset += chunk;
        remaining -= chunk;
        cluster = fat_get(cluster);
    }
    return (int)offset;
}

int fat32_lookup(const char *name83, struct fat32_dirent *out) {
    if (!mounted) return -1;
    struct fat32_dirent entries[FAT32_MAX_FILES];
    int count = fat32_list_root(entries, FAT32_MAX_FILES);
    if (count < 0) return -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name83) == 0) {
            if (out) *out = entries[i];
            return 0;
        }
    }
    return -1;
}

int fat32_lookup_in_dir(uint32_t dir_cluster, const char *name83,
                           struct fat32_dirent *out) {
    if (!mounted || dir_cluster < 2) return -1;
    struct fat32_dirent entries[FAT32_MAX_FILES];
    int count = fat32_list_dir(dir_cluster, entries, FAT32_MAX_FILES);
    if (count < 0) return -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name83) == 0) {
            if (out) *out = entries[i];
            return 0;
        }
    }
    return -1;
}

/* ---- Public API: Write ---- */

int fat32_write_file(uint32_t first_cluster, uint32_t size,
                       const void *buf, uint32_t count, uint32_t offset,
                       uint32_t *out_cluster, uint32_t *out_size) {
    if (!mounted || !blk_write || count == 0) {
        if (out_cluster) *out_cluster = first_cluster;
        if (out_size) *out_size = size;
        return 0;
    }

    /* If new file, allocate first cluster */
    uint32_t cluster = first_cluster;
    if (cluster == 0) {
        cluster = fat32_alloc_cluster();
        if (cluster == 0) return -1;
        first_cluster = cluster;
        if (zero_cluster(cluster) < 0) return -1;
    }

    uint32_t new_size = (offset + count > size) ? offset + count : size;
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t bytes_written = 0;

    while (bytes_written < count) {
        uint32_t target_byte = offset + bytes_written;

        /* Walk chain forward to the cluster containing target_byte */
        uint32_t clust_byte = 0;  /* byte offset of start of `cluster` */
        uint32_t cur = first_cluster;

        /* Fast forward: each cluster holds bpb.cluster_size bytes */
        while (clust_byte + bpb.cluster_size <= target_byte) {
            uint32_t next = fat_get(cur);
            if (next >= 2 && next < 0x0FFFFFF8) {
                cur = next;
                clust_byte += bpb.cluster_size;
            } else {
                /* Need to extend */
                uint32_t nc = fat32_alloc_cluster();
                if (nc == 0) goto done;
                if (zero_cluster(nc) < 0) goto done;
                fat_set(cur, nc);
                cur = nc;
                clust_byte += bpb.cluster_size;
            }
        }

        /* `cur` now contains target_byte at offset (target_byte - clust_byte) */
        uint32_t pos_in_cluster = target_byte - clust_byte;
        uint32_t avail = bpb.cluster_size - pos_in_cluster;
        uint32_t to_write = count - bytes_written;
        if (to_write > avail) to_write = avail;

        /* Read cluster, modify, write back */
        uint64_t coff = cluster_to_offset(cur);
        uint32_t sec = (uint32_t)(coff / 512);
        uint32_t cnt = bpb.cluster_size / 512;
        if (read_sectors(sec, cnt, sec_buf2) < 0) goto done;
        memcpy(sec_buf2 + pos_in_cluster, src + bytes_written, to_write);
        if (write_sectors(sec, cnt, sec_buf2) < 0) goto done;

        bytes_written += to_write;
    }

done:
    if (out_cluster) *out_cluster = first_cluster;
    if (out_size) *out_size = new_size;
    return (int)bytes_written;
}

/* Build a 32-byte FAT32 directory entry from a fat32_dirent. */
static void build_raw_entry(const struct fat32_dirent *de, uint8_t *raw) {
    memset(raw, 0, 32);
    format_83_name(de->name, raw);
    raw[11] = de->attr;
    /* NTReserved */
    raw[12] = 0;
    /* Creation time tenths */
    raw[13] = 0;
    /* Creation time (fake: 12:00:00) */
    raw[14] = 0x00; raw[15] = 0x60;
    /* Creation date (fake: 2026-01-01) */
    raw[16] = 0x21; raw[17] = 0x46;
    /* Last access date */
    raw[18] = 0x21; raw[19] = 0x46;
    /* First cluster high */
    raw[20] = (de->first_cluster >> 16) & 0xFF;
    raw[21] = (de->first_cluster >> 24) & 0xFF;
    /* Last write time */
    raw[22] = 0x00; raw[23] = 0x60;
    /* Last write date */
    raw[24] = 0x21; raw[25] = 0x46;
    /* First cluster low */
    raw[26] = de->first_cluster & 0xFF;
    raw[27] = (de->first_cluster >> 8) & 0xFF;
    /* File size */
    raw[28] = de->file_size & 0xFF;
    raw[29] = (de->file_size >> 8) & 0xFF;
    raw[30] = (de->file_size >> 16) & 0xFF;
    raw[31] = (de->file_size >> 24) & 0xFF;
}

int fat32_create_file(const char *name83, struct fat32_dirent *out) {
    return fat32_create_file_in_dir(bpb.root_cluster, name83, out);
}

int fat32_create_file_in_dir(uint32_t dir_cluster, const char *name83,
                                struct fat32_dirent *out) {
    if (!mounted || !blk_write || !name83) return -1;

    /* Check if already exists */
    struct fat32_dirent existing;
    if (fat32_lookup_in_dir(dir_cluster, name83, &existing) == 0) {
        pr_warn("fat32: '%s' already exists\n", name83);
        return -1;
    }

    struct fat32_dirent de;
    memset(&de, 0, sizeof(de));
    strncpy(de.name, name83, 12);
    de.name[12] = '\0';
    de.attr = 0x20;  /* Archive */
    de.first_cluster = 0;
    de.file_size = 0;
    de.is_dir = 0;

    uint8_t raw[32];
    build_raw_entry(&de, raw);

    if (write_dir_entry(dir_cluster, raw, NULL) < 0) {
        pr_warn("fat32: failed to create '%s' in dir cluster %u\n",
                name83, dir_cluster);
        return -1;
    }

    pr_info("fat32: created '%s' (size=0)\n", name83);
    if (out) *out = de;
    return 0;
}

int fat32_unlink(const char *name83) {
    if (!mounted || !blk_write || !name83) return -1;

    struct fat32_dirent de;
    if (fat32_lookup(name83, &de) < 0) {
        pr_warn("fat32: unlink '%s' not found\n", name83);
        return -1;
    }

    if (de.is_dir) {
        pr_warn("fat32: unlink '%s' is a directory, use rmdir\n", name83);
        return -1;
    }

    /* Free the cluster chain */
    if (de.first_cluster >= 2) {
        fat32_free_chain(de.first_cluster);
    }

    /* Mark the directory entry as deleted (0xE5) */
    uint8_t raw[32];
    build_raw_entry(&de, raw);
    raw[0] = 0xE5;
    if (write_dir_entry(bpb.root_cluster, raw, name83) < 0) {
        pr_warn("fat32: failed to mark '%s' as deleted\n", name83);
        return -1;
    }

    pr_info("fat32: unlinked '%s' (freed cluster chain from %u)\n",
            name83, de.first_cluster);
    return 0;
}

int fat32_mkdir(const char *name83, struct fat32_dirent *out) {
    if (!mounted || !blk_write || !name83) return -1;

    /* Check if already exists */
    struct fat32_dirent existing;
    if (fat32_lookup(name83, &existing) == 0) {
        pr_warn("fat32: '%s' already exists\n", name83);
        return -1;
    }

    /* Allocate a cluster for the new directory */
    uint32_t new_cluster = fat32_alloc_cluster();
    if (new_cluster == 0) {
        pr_warn("fat32: no free cluster for directory\n");
        return -1;
    }
    if (zero_cluster(new_cluster) < 0) {
        fat_set(new_cluster, 0);
        return -1;
    }

    /* Create "." and ".." entries in the new directory cluster */
    uint8_t cluster_data[512 * 4];
    memset(cluster_data, 0, sizeof(cluster_data));

    /* "." entry — points to self */
    struct fat32_dirent dot;
    memset(&dot, 0, sizeof(dot));
    strcpy(dot.name, ".");
    dot.attr = 0x10;  /* Directory */
    dot.first_cluster = new_cluster;
    dot.file_size = 0;
    dot.is_dir = 1;
    build_raw_entry(&dot, cluster_data);

    /* ".." entry — points to parent (root) */
    struct fat32_dirent dotdot;
    memset(&dotdot, 0, sizeof(dotdot));
    strcpy(dotdot.name, "..");
    dotdot.attr = 0x10;
    dotdot.first_cluster = bpb.root_cluster;
    dotdot.file_size = 0;
    dotdot.is_dir = 1;
    build_raw_entry(&dotdot, cluster_data + 32);

    /* Write the cluster */
    uint64_t off = cluster_to_offset(new_cluster);
    uint32_t sec = (uint32_t)(off / 512);
    uint32_t cnt = bpb.cluster_size / 512;
    if (write_sectors(sec, cnt, cluster_data) < 0) {
        fat_set(new_cluster, 0);
        return -1;
    }

    /* Add entry for the new directory in the parent (root) */
    struct fat32_dirent de;
    memset(&de, 0, sizeof(de));
    strncpy(de.name, name83, 12);
    de.name[12] = '\0';
    de.attr = 0x10;  /* Directory */
    de.first_cluster = new_cluster;
    de.file_size = 0;
    de.is_dir = 1;

    uint8_t raw[32];
    build_raw_entry(&de, raw);
    if (write_dir_entry(bpb.root_cluster, raw, NULL) < 0) {
        fat_set(new_cluster, 0);
        return -1;
    }

    pr_info("fat32: created directory '%s' (cluster %u)\n", name83, new_cluster);
    if (out) *out = de;
    return 0;
}

int fat32_update_entry_size(const char *name83, uint32_t new_size) {
    if (!mounted || !blk_write || !name83) return -1;

    struct fat32_dirent de;
    if (fat32_lookup(name83, &de) < 0) return -1;

    de.file_size = new_size;

    uint8_t raw[32];
    build_raw_entry(&de, raw);
    if (write_dir_entry(bpb.root_cluster, raw, name83) < 0) return -1;

    return 0;
}

int fat32_update_entry(const char *name83, uint32_t new_cluster, uint32_t new_size) {
    if (!mounted || !blk_write || !name83) return -1;

    struct fat32_dirent de;
    if (fat32_lookup(name83, &de) < 0) return -1;

    de.first_cluster = new_cluster;
    de.file_size = new_size;

    uint8_t raw[32];
    build_raw_entry(&de, raw);
    if (write_dir_entry(bpb.root_cluster, raw, name83) < 0) return -1;

    return 0;
}
