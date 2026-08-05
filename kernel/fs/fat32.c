/*
 * Lestra OS - FAT32 Read-Only Filesystem Driver
 * Copyright (c) 2026 lestramk.org
 *
 * Adapted from OSdev FAT32 tutorial concepts, rewritten for lestraOS's
 * driver model. Uses a function-pointer-based block read interface
 * so it can work with VirtIO-blk, AHCI, or any future block driver.
 */

#include <lestra/fat32.h>
#include <lestra/printk.h>
#include <string.h>

/* Sector read buffer — reused across calls to avoid stack allocation. */
static uint8_t sec_buf[512];

/* Block device read function (set by fat32_init). */
static fat32_read_fn blk_read;

/* Parsed BPB */
static struct fat32_bpb bpb;

/* Mount state */
static int mounted = 0;

/* Read a single sector into the static buffer. Returns 0 on success. */
static int read_sector(uint32_t lba) {
    if (blk_read(lba, 1, sec_buf) < 0) return -1;
    return 0;
}

/* Read N sectors into caller-supplied buffer. */
static int read_sectors(uint32_t lba, uint32_t count, void *buf) {
    if (blk_read(lba, count, buf) < 0) return -1;
    return 0;
}

/* Get FAT entry for a given cluster. Returns 0x0FFFFFFF on EOC/error. */
static uint32_t fat_get(uint32_t cluster) {
    if (cluster < 2) return 0x0FFFFFFF;
    /* Each FAT entry is 4 bytes. Figure out which sector + offset. */
    uint32_t fat_byte = bpb.fat_offset + cluster * 4;
    uint32_t fat_sec  = fat_byte / 512;
    uint32_t fat_off  = fat_byte % 512;
    if (read_sector(fat_sec) < 0) return 0x0FFFFFFF;
    uint32_t entry;
    memcpy(&entry, sec_buf + fat_off, 4);
    return entry & 0x0FFFFFFF;
}

/* Convert cluster number to byte offset in the image. */
static uint64_t cluster_to_offset(uint32_t cluster) {
    return bpb.data_offset + (uint64_t)(cluster - 2) * bpb.cluster_size;
}

/* Parse a raw 32-byte directory entry into our struct. */
static int parse_dirent(const uint8_t *raw, struct fat32_dirent *out) {
    if (raw[0] == 0x00) return -1;   /* no more entries */
    if (raw[0] == 0xE5) return -2;  /* deleted */
    uint8_t attr = raw[11];
    if (attr == 0x0F) return -3;     /* LFN entry — skip */

    /* Extract 8.3 name */
    char name[12];
    memcpy(name, raw, 11);
    name[11] = '\0';

    /* Format: "HELLO   TXT" -> "HELLO.TXT" */
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

/* Read all directory entries from a cluster chain into the buffer.
 * Returns raw byte count, or <0 on error. */
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

/* ---- Public API ---- */

int fat32_init(fat32_read_fn read_fn) {
    if (!read_fn) return -1;
    blk_read = read_fn;

    /* Read BPB from sector 0 */
    if (read_sector(0) < 0) {
        pr_warn("fat32: failed to read boot sector\n");
        return -1;
    }

    /* Quick sanity: jump instruction, bytes/sector = 512, cluster = 1-128 */
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

    uint32_t fat_size;
    memcpy(&fat_size, sec_buf + 36, 4);
    bpb.fat_offset = bpb.reserved_sectors * 512;
    bpb.data_offset = bpb.fat_offset + fat_size * 512;
    bpb.cluster_size = bpb.bytes_per_sector * bpb.sectors_per_cluster;

    memcpy(&bpb.root_cluster, sec_buf + 44, 4);

    uint32_t total_sec;
    memcpy(&total_sec, sec_buf + 32, 4);
    if (total_sec == 0) {
        uint16_t ts;
        memcpy(&ts, sec_buf + 19, 2);
        total_sec = ts;
    }
    uint32_t data_sec = total_sec - bpb.reserved_sectors - fat_size;
    bpb.total_clusters = data_sec / bpb.sectors_per_cluster;

    mounted = 1;
    pr_info("fat32: mounted (bps=%u, spc=%u, root=%u, clusters=%u, data@0x%x)\n",
            bpb.bytes_per_sector, bpb.sectors_per_cluster,
            bpb.root_cluster, bpb.total_clusters, bpb.data_offset);
    return 0;
}

int fat32_is_mounted(void) {
    return mounted;
}

int fat32_list_root(struct fat32_dirent *out, int max) {
    if (!mounted) return -1;
    /* Read root directory cluster chain into a buffer.
     * Root dir can span multiple clusters; allocate enough for 8 clusters. */
    static uint8_t dir_buf[8 * 512 * 4];  /* 16 KB, enough for most root dirs */
    int raw = read_dir_cluster_chain(bpb.root_cluster, dir_buf, sizeof(dir_buf));
    if (raw < 0) return -1;

    int count = 0;
    for (int off = 0; off + 32 <= raw && count < max; off += 32) {
        const uint8_t *entry = dir_buf + off;
        if (entry[0] == 0x00) break;           /* end of dir */
        if (entry[0] == 0xE5) continue;        /* deleted */
        uint8_t attr = entry[11];
        if (attr == 0x0F) continue;             /* LFN slot */
        if (attr & 0x08) continue;             /* volume label */

        int rc = parse_dirent(entry, &out[count]);
        if (rc == 0) count++;
    }
    return count;
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

    /* name83 can be either "HELLO.TXT" or "HELLO   TXT" */
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name83) == 0) {
            if (out) *out = entries[i];
            return 0;
        }
    }
    return -1;
}
