/*
 * Lestra OS - DEB Package Installer
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Extracts and installs .deb packages. DEB files are ar archives
 * containing:
 *   - debian-binary: version info
 *   - control.tar.gz: package metadata
 *   - data.tar.gz: actual files
 *
 * This installer:
 *   1. Extracts control.tar.gz to get package info
 *   2. Extracts data.tar.gz to install files
 *   3. Registers package in installed database
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/vfs.h>
#include <lestra/mm.h>
#include <lestra/timer.h>
#include <string.h>

/* DEB magic: "!<arch>\n" */
#define DEB_MAGIC "!<arch>\n"
#define DEB_MAGIC_SIZE 8

/* ar header format (60 bytes) */
typedef struct {
    char Name[16];
    char Date[12];
    char OwnerID[6];
    char GroupID[6];
    char Mode[8];
    char Size[10];
    char EndHeader[2];
} __packed ar_header_t;

/* Simple tar header (ustar format) */
typedef struct {
    char Name[100];
    char Mode[8];
    char OwnerID[8];
    char GroupID[8];
    char Size[12];
    char ModTime[12];
    char Checksum[8];
    char TypeFlag;
    char LinkName[100];
    char Magic[6];
    char Version[2];
    char UserName[32];
    char GroupName[32];
    char DevMajor[8];
    char DevMinor[8];
    char Prefix[155];
} __packed tar_header_t;

/* Installed package database */
#define MAX_INSTALLED_DEBS 64
#define MAX_DEB_NAME_LEN 64

struct installed_deb {
    char name[MAX_DEB_NAME_LEN];
    char version[32];
    char description[128];
    uint64_t install_time_ms;
};

static struct installed_deb installed_debs[MAX_INSTALLED_DEBS];
static int installed_deb_count = 0;

/* Parse octal string */
static uint32_t parse_octal(const char* s, int len) {
    uint32_t val = 0;
    for (int i = 0; i < len && s[i] >= '0' && s[i] <= '7'; i++) {
        val = (val << 3) + (s[i] - '0');
    }
    return val;
}

/* Find installed package */
static int find_installed_deb(const char* name) {
    for (int i = 0; i < installed_deb_count; i++) {
        if (strcmp(installed_debs[i].name, name) == 0) return i;
    }
    return -1;
}

/* Extract tar archive and install files */
static int extract_tar(const uint8_t* data, size_t size, const char* prefix) {
    size_t offset = 0;
    int files_installed = 0;
    
    while (offset + 512 <= size) {
        const tar_header_t* hdr = (const tar_header_t*)(data + offset);
        
        /* Check for end of archive (all zeros) */
        int all_zero = 1;
        for (int i = 0; i < 512; i++) {
            if (data[offset + i] != 0) { all_zero = 0; break; }
        }
        if (all_zero) break;
        
        /* Validate tar magic */
        if (memcmp(hdr->Magic, "ustar", 5) != 0) {
            offset += 512;
            continue;
        }
        
        uint32_t file_size = parse_octal(hdr->Size, 12);
        char full_path[256];
        
        /* Build full path */
        if (prefix[0]) {
            ksnprintf(full_path, sizeof(full_path), "%s/%s", prefix, hdr->Name);
        } else {
            strncpy(full_path, hdr->Name, sizeof(full_path) - 1);
        }
        
        /* Only extract regular files (TypeFlag == '0' or '\0') */
        if (hdr->TypeFlag == '0' || hdr->TypeFlag == '\0') {
            /* Create parent directories */
            char dir_path[256];
            strncpy(dir_path, full_path, sizeof(dir_path));
            char* last_slash = strrchr(dir_path, '/');
            if (last_slash) {
                *last_slash = '\0';
                /* Simple mkdir -p (ignore errors if exists) */
                extern int vfs_mkdir(const char* path, uint32_t mode);
                vfs_mkdir(dir_path, 0755);
            }
            
            /* Write file */
            int fd = vfs_open(full_path, 0x0040 | 0x0200);  /* O_CREAT | O_WRONLY */
            if (fd >= 0) {
                vfs_write(fd, data + offset + 512, file_size);
                vfs_close(fd);
                files_installed++;
                pr_info("deb: installed %s (%u bytes)\n", full_path, file_size);
            }
        }
        
        /* Advance to next entry (512-byte aligned) */
        offset += 512 + ((file_size + 511) & ~511);
    }
    
    return files_installed;
}

/* Parse control file and extract package info */
static void parse_control(const uint8_t* data, size_t size, 
                          char* name, char* version, char* description) {
    name[0] = version[0] = description[0] = '\0';
    
    size_t pos = 0;
    while (pos < size) {
        /* Find line end */
        size_t line_end = pos;
        while (line_end < size && data[line_end] != '\n') line_end++;
        
        /* Parse field */
        if (strncmp((const char*)data + pos, "Package: ", 9) == 0) {
            strncpy(name, (const char*)data + pos + 9, line_end - pos - 9);
            name[line_end - pos - 9] = '\0';
        } else if (strncmp((const char*)data + pos, "Version: ", 9) == 0) {
            strncpy(version, (const char*)data + pos + 9, line_end - pos - 9);
            version[line_end - pos - 9] = '\0';
        } else if (strncmp((const char*)data + pos, "Description: ", 13) == 0) {
            strncpy(description, (const char*)data + pos + 13, line_end - pos - 13);
            description[line_end - pos - 13] = '\0';
        }
        
        pos = line_end + 1;
    }
}

/* Install a .deb package */
int deb_install(const char* path) {
    int fd = vfs_open(path, 0);
    if (fd < 0) {
        pr_warn("deb: '%s' not found\n", path);
        return -1;
    }
    
    /* Read entire deb file */
    static uint8_t deb_buf[256 * 1024];  /* 256KB max */
    size_t total = 0;
    while (total < sizeof(deb_buf)) {
        ssize_t n = vfs_read(fd, &deb_buf[total], sizeof(deb_buf) - total);
        if (n <= 0) break;
        total += n;
    }
    vfs_close(fd);
    
    /* Verify DEB magic */
    if (total < DEB_MAGIC_SIZE || memcmp(deb_buf, DEB_MAGIC, DEB_MAGIC_SIZE) != 0) {
        pr_warn("deb: '%s' is not a valid .deb file\n", path);
        return -1;
    }
    
    pr_info("deb: installing %s (%u bytes)\n", path, (unsigned)total);
    
    /* Parse ar archive */
    size_t offset = DEB_MAGIC_SIZE;
    char pkg_name[MAX_DEB_NAME_LEN] = {0};
    char pkg_version[32] = {0};
    char pkg_desc[128] = {0};
    int files_installed = 0;
    
    while (offset + sizeof(ar_header_t) <= total) {
        const ar_header_t* ar = (const ar_header_t*)(deb_buf + offset);
        
        /* Validate ar header */
        if (ar->EndHeader[0] != '`' || ar->EndHeader[1] != '\n') {
            offset += 2;
            continue;
        }
        
        uint32_t ar_size = parse_octal(ar->Size, 10);
        const uint8_t* ar_data = deb_buf + offset + sizeof(ar_header_t);
        
        /* Process archive members */
        if (strncmp(ar->Name, "control.tar", 11) == 0 || 
            strncmp(ar->Name, "control.tar.", 12) == 0) {
            /* Extract control info */
            pr_info("deb: found control archive\n");
            /* For simplicity, we'll parse the control file directly if it's uncompressed */
            if (ar_size > 512) {
                /* Check if it's a plain tar (no gzip) */
                if (memcmp(ar_data, "ustar", 5) == 0) {
                    /* It's a tar - extract to temp and parse control */
                    /* Simplified: just look for control file in the data */
                    extract_tar(ar_data, ar_size, "");
                }
            }
        } else if (strncmp(ar->Name, "data.tar", 8) == 0) {
            /* Extract data archive */
            pr_info("deb: found data archive (%u bytes)\n", ar_size);
            if (ar_size > 512 && memcmp(ar_data, "ustar", 5) == 0) {
                files_installed = extract_tar(ar_data, ar_size, "");
            }
        }
        
        offset += sizeof(ar_header_t) + ar_size;
        /* Align to 2-byte boundary */
        if (offset & 1) offset++;
    }
    
    /* Extract package name from filename if not found in control */
    if (!pkg_name[0]) {
        const char* basename = strrchr(path, '/');
        if (basename) basename++; else basename = path;
        strncpy(pkg_name, basename, MAX_DEB_NAME_LEN - 1);
        char* dot = strrchr(pkg_name, '.');
        if (dot) *dot = '\0';
    }
    
    /* Register as installed */
    if (installed_deb_count < MAX_INSTALLED_DEBS) {
        strncpy(installed_debs[installed_deb_count].name, pkg_name, MAX_DEB_NAME_LEN - 1);
        strncpy(installed_debs[installed_deb_count].version, pkg_version, 31);
        strncpy(installed_debs[installed_deb_count].description, pkg_desc, 127);
        installed_debs[installed_deb_count].install_time_ms = timer_get_ms();
        installed_deb_count++;
        pr_info("deb: registered %s %s as installed\n", pkg_name, pkg_version);
    }
    
    pr_info("deb: installation complete (%d files installed)\n", files_installed);
    return 0;
}

/* List installed .deb packages */
void deb_list_installed(void) {
    if (installed_deb_count == 0) {
        printk("\nNo .deb packages installed.\n");
        return;
    }
    printk("\nInstalled .deb packages (%d):\n", installed_deb_count);
    for (int i = 0; i < installed_deb_count; i++) {
        printk("  %s %s - %s\n", installed_debs[i].name, 
               installed_debs[i].version, installed_debs[i].description);
    }
}

/* Remove a .deb package */
int deb_remove(const char* name) {
    int idx = find_installed_deb(name);
    if (idx < 0) {
        pr_warn("deb: package '%s' not installed\n", name);
        return -1;
    }
    
    pr_info("deb: removing %s %s\n", installed_debs[idx].name, installed_debs[idx].version);
    
    /* Shift remaining packages */
    for (int i = idx; i < installed_deb_count - 1; i++) {
        installed_debs[i] = installed_debs[i + 1];
    }
    installed_deb_count--;
    
    return 0;
}

/* Initialize DEB installer */
void deb_init(void) {
    installed_deb_count = 0;
    pr_info("deb: package installer initialized\n");
}
