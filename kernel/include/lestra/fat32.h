/*
 * Lestra OS - FAT32 Read-Only Filesystem Driver
 * Copyright (c) 2026 lestramk.org
 *
 * Parses FAT32 BPB, walks FAT chains, reads directory entries and file data.
 * Designed to work with any block device providing sector-level reads.
 */

#ifndef LESTRA_FAT32_H
#define LESTRA_FAT32_H

#include <lestra/types.h>

#define FAT32_MAX_NAME  256
#define FAT32_MAX_FILES  64

/* Parsed BPB fields */
struct fat32_bpb {
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_offset;          /* byte offset of first FAT */
    uint32_t data_offset;         /* byte offset of data region */
    uint32_t root_cluster;        /* first cluster of root dir */
    uint32_t cluster_size;        /* bytes per cluster */
    uint32_t total_clusters;
};

/* A single directory entry (file or subdirectory) */
struct fat32_dirent {
    char    name[13];             /* 8.3 name: "FILENAMEEXT" */
    uint8_t attr;
    uint32_t first_cluster;
    uint32_t file_size;
    int     is_dir;
};

/* Block device read function signature */
typedef int (*fat32_read_fn)(uint64_t lba, uint32_t count, void *buf);

/* Initialize: parse BPB from sector 0 via the given read function. */
int  fat32_init(fat32_read_fn read_fn);

/* List files in root directory. Returns count, or <0 on error. */
int  fat32_list_root(struct fat32_dirent *out, int max);

/* Read an entire file (by cluster chain) into buf.
 * Returns bytes read, or <0 on error. */
int  fat32_read_file(uint32_t first_cluster, uint32_t size,
                       void *buf, uint32_t bufsize);

/* Look up a file by 8.3 name in the root directory.
 * Returns 0 on success, fills *out. */
int  fat32_lookup(const char *name83, struct fat32_dirent *out);

/* Check if a FAT32 volume was successfully mounted. */
int  fat32_is_mounted(void);

#endif /* LESTRA_FAT32_H */
