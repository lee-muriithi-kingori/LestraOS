/*
 * Lestra OS - TLS runtime (per-thread Thread-Local Storage)
 * Copyright (c) 2026 lestramk.org
 */
#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/mm.h>
#include <string.h>

#define MAX_TLS_MODULES 64

struct tls_module {
    int in_use;
    uint64_t module_id;
    void* init_image;
    size_t init_size;
    size_t total_size;
    size_t alignment;
    int64_t offset_from_tcb;
};

static struct tls_module tls_modules[MAX_TLS_MODULES];
static int tls_next_module_id = 1;
static void* tls_tcb = NULL;
static size_t tls_total_size = 0;

uint64_t tls_register_module(void* init_image, size_t init_size,
                              size_t total_size, size_t alignment) {
    if (alignment == 0) alignment = 16;
    if (alignment > 64) alignment = 64;
    for (int i = 0; i < MAX_TLS_MODULES; i++) {
        if (!tls_modules[i].in_use) {
            tls_modules[i].in_use = 1;
            tls_modules[i].module_id = (uint64_t)tls_next_module_id++;
            tls_modules[i].init_image = init_image;
            tls_modules[i].init_size = init_size;
            tls_modules[i].total_size = total_size;
            tls_modules[i].alignment = alignment;
            tls_modules[i].offset_from_tcb = 0;
            return tls_modules[i].module_id;
        }
    }
    return 0;
}

int tls_setup(void) {
    if (tls_tcb) return 0;
    size_t tcb_size = 1024;
    size_t total = 0;
    int64_t current_offset = 0;
    int max_id = tls_next_module_id - 1;

    for (int id = max_id; id >= 1; id--) {
        struct tls_module* m = NULL;
        for (int i = 0; i < MAX_TLS_MODULES; i++) {
            if (tls_modules[i].in_use && tls_modules[i].module_id == (uint64_t)id) {
                m = &tls_modules[i]; break;
            }
        }
        if (!m) continue;
        size_t align = m->alignment;
        size_t misalign = ((size_t)(-current_offset)) % align;
        if (misalign) current_offset -= (int64_t)(align - misalign);
        current_offset -= (int64_t)m->total_size;
        m->offset_from_tcb = current_offset;
        total += m->total_size;
    }

    tls_total_size = total + tcb_size;
    tls_tcb = kmalloc(tls_total_size);
    if (!tls_tcb) return -1;
    memset(tls_tcb, 0, tls_total_size);

    void* tcb_ptr = (uint8_t*)tls_tcb + total;
    for (int id = 1; id <= max_id; id++) {
        struct tls_module* m = NULL;
        for (int i = 0; i < MAX_TLS_MODULES; i++) {
            if (tls_modules[i].in_use && tls_modules[i].module_id == (uint64_t)id) {
                m = &tls_modules[i]; break;
            }
        }
        if (!m) continue;
        if (m->init_size > 0 && m->init_image) {
            void* block = (uint8_t*)tcb_ptr + m->offset_from_tcb;
            memcpy(block, m->init_image, m->init_size);
        }
    }

    uint64_t fs_base = (uint64_t)tcb_ptr;
    __asm__ volatile("wrmsr" ::
        "c"((uint32_t)0xC0000100u),
        "a"((uint32_t)(fs_base & 0xFFFFFFFFu)),
        "d"((uint32_t)(fs_base >> 32)));
    *(void**)tcb_ptr = tcb_ptr;
    return 0;
}

int64_t tls_tpoff(uint64_t module_id, uint64_t symbol_value) {
    for (int i = 0; i < MAX_TLS_MODULES; i++) {
        if (tls_modules[i].in_use && tls_modules[i].module_id == module_id)
            return tls_modules[i].offset_from_tcb + (int64_t)symbol_value;
    }
    return 0;
}

struct tls_index { uint64_t ti_module; uint64_t ti_offset; };

void* __tls_get_addr(struct tls_index* ti) {
    if (!ti || !tls_tcb) return NULL;
    void* tcb_ptr = (uint8_t*)tls_tcb + (tls_total_size - 1024);
    int64_t mod_offset = 0;
    for (int i = 0; i < MAX_TLS_MODULES; i++) {
        if (tls_modules[i].in_use && tls_modules[i].module_id == ti->ti_module) {
            mod_offset = tls_modules[i].offset_from_tcb; break;
        }
    }
    return (void*)((uint8_t*)tcb_ptr + mod_offset + (int64_t)ti->ti_offset);
}

int tls_is_initialized(void) { return tls_tcb != NULL; }
void* tls_get_tcb(void) {
    if (!tls_tcb) return NULL;
    return (uint8_t*)tls_tcb + (tls_total_size - 1024);
}

void* tls_allocate_thread(void) {
    if (!tls_tcb) return NULL;
    void* new_area = kmalloc(tls_total_size);
    if (!new_area) return NULL;
    memcpy(new_area, tls_tcb, tls_total_size);
    void* new_tcb = (uint8_t*)new_area + (tls_total_size - 1024);
    *(void**)new_tcb = new_tcb;
    return new_tcb;
}

void tls_free_thread(void* tcb) {
    if (!tcb) return;
    void* base = (uint8_t*)tcb - (tls_total_size - 1024);
    kfree(base);
}
