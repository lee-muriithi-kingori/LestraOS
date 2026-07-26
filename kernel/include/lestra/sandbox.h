/*
 * Lestra OS - Sandbox API
 * Copyright (c) 2026 lestramk.org
 *
 * Isolated execution environments with resource limits, network
 * restrictions, and filesystem isolation. Max 2 concurrent sandboxes.
 */

#ifndef LESTRA_SANDBOX_H
#define LESTRA_SANDBOX_H

#include <lestra/types.h>

#define SANDBOX_MAX         2
#define SANDBOX_NAME_LEN    32
#define SANDBOX_ROOTFS_LEN  128
#define SANDBOX_CMD_LEN     256
#define SANDBOX_PATH_LEN    128

#define SANDBOX_DEFAULT_MEM_LIMIT   (64 * 1024 * 1024)  /* 64 MB */
#define SANDBOX_DEFAULT_MAX_FDS     32

#define SANDBOX_DEFAULT_STORAGE_SIZE  (16 * 1024 * 1024)  /* 16 MB */
#define SANDBOX_MIN_STORAGE_SIZE      (1  * 1024 * 1024)  /*  1 MB */
#define SANDBOX_MAX_STORAGE_SIZE      (256 * 1024 * 1024) /* 256 MB */

struct sandbox {
    int     in_use;
    int     id;                     /* 1 or 2 */
    int     pid;                    /* process running in sandbox */
    char    name[SANDBOX_NAME_LEN];
    uint64_t* pml4;                /* isolated page table */
    uint64_t memory_limit;         /* max memory (bytes) */
    uint64_t memory_used;          /* current memory */
    int     network_disabled;      /* 1 = no network access */
    int     max_open_fds;          /* max file descriptors */
    char    rootfs[SANDBOX_ROOTFS_LEN]; /* sandbox root filesystem path */
    int     port;                  /* HTTP port for sandbox access */
    int     active;                /* 1 = running */
    uint64_t storage_size;         /* disk image size in bytes */
    char    disk_image_path[SANDBOX_PATH_LEN]; /* host ext2 path to disk image */
    int     storage_mounted;       /* 1 if storage is active */
};

struct sandbox_info {
    int     id;
    int     pid;
    char    name[SANDBOX_NAME_LEN];
    int     active;
    int     network_disabled;
    uint64_t memory_limit;
    uint64_t memory_used;
    int     max_open_fds;
    char    rootfs[SANDBOX_ROOTFS_LEN];
    int     port;
    uint64_t storage_size;
    int     storage_mounted;
};

/* Sandbox management */
void     sandbox_init(void);
int      sandbox_create(const char* name, int port, uint64_t storage_size);
int      sandbox_destroy(int id);
int      sandbox_start(int id, const char* cmd);
int      sandbox_stop(int id);
int      sandbox_status(int id, struct sandbox_info* info);
int      sandbox_is_sandboxed(int pid);
int      sandbox_count(void);
void     sandbox_list(void);

/* Sandbox server */
void     sandbox_server_start(int port);
void     sandbox_server_start_tls(int port);
void     sandbox_server_stop(void);
int      sandbox_server_is_running(void);
int      sandbox_server_port(void);

#endif /* LESTRA_SANDBOX_H */
