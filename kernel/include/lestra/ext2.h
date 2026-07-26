#ifndef _LESTRA_EXT2_H
#define _LESTRA_EXT2_H

#include <lestra/types.h>

int      ext2_mount(void);
int      ext2_is_mounted(void);
int      ext2_is_writable(void);
uint32_t ext2_resolve_path(const char* path);
int      ext2_read_file(const char* path, void* buf, uint32_t bufsize);
int      ext2_write_file(const char* path, const void* buf, uint32_t len);
uint32_t ext2_create_file(const char* path, uint16_t mode);
uint32_t ext2_mkdir(const char* path, uint16_t mode);
int      ext2_unlink(const char* path);
void     ext2_list_root(void (*callback)(const char* name, uint32_t inode, uint8_t type));
void     ext2_list_dir(const char* path, void (*callback)(const char* name, uint32_t inode, uint8_t type));
uint16_t ext2_get_inode_mode(const char* path);

#endif
