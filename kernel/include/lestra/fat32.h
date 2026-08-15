/*
 * Lestra OS - FAT32 Read-Write Filesystem Driver
 * Copyright (c) 2026 lestramk.org
 *
 * Parses FAT32 BPB, walks FAT chains, reads/writes directory entries
 * and file data. KE-24 adds full write support: cluster allocation,
 * file creation, file writing, directory creation, and file deletion.
 *
 * Uses function-pointer-based block I/O so it works with VirtIO-blk,
 * AHCI, or any future block driver.
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
    uint32_t fat_size_sectors;    /* sectors per FAT copy */
};

/* A single directory entry (file or subdirectory) */
struct fat32_dirent {
    char    name[13];             /* 8.3 name: "FILENAMEEXT" */
    uint8_t attr;
    uint32_t first_cluster;
    uint32_t file_size;
    int     is_dir;
};

/* Block device I/O function signatures */
typedef int (*fat32_read_fn)(uint64_t lba, uint32_t count, void *buf);
typedef int (*fat32_write_fn)(uint64_t lba, uint32_t count, const void *buf);

/* Initialize: parse BPB from sector 0 via the given read function. */
int  fat32_init(fat32_read_fn read_fn);

/* Set the write function. Must be called after fat32_init if writes
 * are desired. If not called, all write operations return -1. */
void fat32_set_write_fn(fat32_write_fn write_fn);

/* Check if write support is available. */
int  fat32_is_writable(void);

/* List files in root directory. Returns count, or <0 on error. */
int  fat32_list_root(struct fat32_dirent *out, int max);

/* List files in a subdirectory by its starting cluster.
 * Returns count, or <0 on error. */
int  fat32_list_dir(uint32_t dir_cluster, struct fat32_dirent *out, int max);

/* Read an entire file (by cluster chain) into buf.
 * Returns bytes read, or <0 on error. */
int  fat32_read_file(uint32_t first_cluster, uint32_t size,
                       void *buf, uint32_t bufsize);

/* Write data to a file, extending the cluster chain if needed.
 * first_cluster: starting cluster (0 for new files).
 * size: current file size.
 * offset: byte offset within file to start writing.
 * On return, *out_cluster = first cluster, *out_size = new file size.
 * Returns bytes written, or <1 on error. */
int  fat32_write_file(uint32_t first_cluster, uint32_t size,
                       const void *buf, uint32_t count, uint32_t offset,
                       uint32_t *out_cluster, uint32_t *out_size);

/* Create a new empty file in the root directory.
 * Returns 0 on success, fills *out with the new entry. */
int  fat32_create_file(const char *name83, struct fat32_dirent *out);

/* Create a new empty file in a subdirectory.
 * Returns 0 on success, fills *out with the new entry. */
int  fat32_create_file_in_dir(uint32_t dir_cluster, const char *name83,
                                struct fat32_dirent *out);

/* Delete a file from the root directory by 8.3 name.
 * Frees its cluster chain. Returns 0 on success. */
int  fat32_unlink(const char *name83);

/* Create a subdirectory in the root directory.
 * Returns 0 on success, fills *out with the new entry. */
int  fat32_mkdir(const char *name83, struct fat32_dirent *out);

/* Look up a file by 8.3 name in the root directory.
 * Returns 0 on success, fills *out. */
int  fat32_lookup(const char *name83, struct fat32_dirent *out);

/* Look up a file by 8.3 name in a subdirectory.
 * Returns 0 on success, fills *out. */
int  fat32_lookup_in_dir(uint32_t dir_cluster, const char *name83,
                           struct fat32_dirent *out);

/* Check if a FAT32 volume was successfully mounted. */
int  fat32_is_mounted(void);

/* Get the BPB (for shim layer). */
const struct fat32_bpb* fat32_get_bpb(void);

/* Update the file size field of a directory entry on disk.
 * Used by the shim after a write to keep the dirent current. */
int  fat32_update_entry_size(const char *name83, uint32_t new_size);

/* Update both cluster and size of a directory entry on disk. */
int  fat32_update_entry(const char *name83, uint32_t new_cluster, uint32_t new_size);

#endif /* LESTRA_FAT32_H */
