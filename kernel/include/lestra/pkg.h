/*
 * Lestra OS - Package Manager API
 * Copyright (c) 2026 lestramk.org
 */
#ifndef LESTRA_PKG_H
#define LESTRA_PKG_H

#include <lestra/types.h>

/* Initialize the package manager (call once at boot) */
void pkg_init(void);

/* Install a package by name. Returns 0 on success. */
int pkg_install(const char* name);

/* Remove an installed package. Returns 0 on success. */
int pkg_remove(const char* name);

/* List all available packages in the catalog. */
void pkg_list_available(void);

/* List installed packages only. */
void pkg_list_installed(void);

/* Search the catalog by name/description substring. */
int pkg_search(const char* query);

/* Show detailed info about a package. */
void pkg_info(const char* name);

/* Repository management */
void pkg_repo_list(void);
int pkg_repo_add(const char* name, const char* url);
int pkg_repo_remove(const char* name);
void pkg_repo_update(void);
int pkg_repo_count(void);

/* Counts */
int pkg_count_installed(void);
int pkg_count_available(void);

/* Alias used by kernel_main splash reporting. */
int pkg_catalog_size(void);

/* ===== DRIVER CATALOG ===== */
typedef enum {
    DRIVER_STATUS_LOADED,
    DRIVER_STATUS_STUB,
    DRIVER_STATUS_MISSING_DEP,
} driver_status_t;

struct driver_entry {
    const char* id;
    const char* name;
    const char* category;
    driver_status_t status;
    const char* description;
};

void pkg_driver_list(void);
const struct driver_entry* pkg_driver_get(int idx);
int pkg_driver_count(void);

/* ===== PREINSTALLED APP CATALOG ===== */
typedef enum {
    APP_KIND_NATIVE  = 0,
    APP_KIND_BUNDLED = 1,
} app_kind_t;

struct preinstalled_app {
    const char* id;
    const char* name;
    const char* icon;
    const char* category;
    const char* version;
    const char* path;
    const char* description;
    app_kind_t kind;
    uint32_t size_kb;
};

void pkg_preinstalled_list(void);
const struct preinstalled_app* pkg_preinstalled_get(int idx);
const struct preinstalled_app* pkg_preinstalled_find(const char* id);
int pkg_preinstalled_count(void);

/* Launcher for preinstalled apps - called by GUI when icon clicked */
const char* pkg_preinstalled_launch(const char* id);

#endif /* LESTRA_PKG_H */
