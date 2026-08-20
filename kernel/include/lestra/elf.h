/*
 * Lestra OS - ELF/PE Loader Common Definitions
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 */

#ifndef LESTRA_ELF_H
#define LESTRA_ELF_H

#include <lestra/types.h>

/* Page mapping functions used by both ELF and PE loaders */
void user_map_page(uintptr_t* pml4, uint64_t vaddr, uint64_t phys, uint64_t flags);
void user_map_data(uintptr_t* pml4, uint64_t vaddr, const void* data, 
                   size_t data_len, int writable, int executable);

#endif /* LESTRA_ELF_H */
