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

/* Counts */
int pkg_count_installed(void);
int pkg_count_available(void);

#endif /* LESTRA_PKG_H */
