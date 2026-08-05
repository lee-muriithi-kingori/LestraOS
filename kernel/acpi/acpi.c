/*
 * Lestra OS - ACPI Table Discovery
 * Copyright (c) 2026 lestramk.org
 *
 * Discovers ACPI tables from physical memory:
 *   RSDP → RSDT/XSDT → MADT, HPET, FACP
 *
 * The first 1GB is identity-mapped (boot.asm 2MB huge pages),
 * so all table physical addresses below 1GB are directly dereferencable.
 * Tables above 1GB are skipped with a warning (would need ioremap).
 *
 * Extracted and extended from the shell.c poweroff command's
 * inline ACPI parsing (which only found FACP for S5 shutdown).
 */

#include <lestra/types.h>
#include <lestra/acpi.h>
#include <lestra/printk.h>
#include <string.h>

/* ----- Global ACPI database ----- */
struct acpi_info g_acpi;

/* ----- Checksum validation ----- */
static int acpi_checksum_valid(const void* table, uint32_t len) {
    const uint8_t* p = (const uint8_t*)table;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum += p[i];
    return (sum == 0);
}

/* ----- Safe physical pointer check -----
 * Only dereference physical addresses below 1GB (identity-mapped region). */
static int phys_ok(uintptr_t addr) {
    return addr < 0x40000000ULL;  /* 1GB */
}

/* ----- RSDP Discovery -----
 * Searches EBDA (first 1KB) and BIOS ROM (0xE0000-0xFFFFF)
 * on 16-byte boundaries for the "RSD PTR " signature. */
static const struct acpi_rsdp* find_rsdp(void) {
    /* Method 1: EBDA */
    uint16_t ebda_seg = *(volatile uint16_t*)(uintptr_t)0x40E;
    uintptr_t ebda_base = (uintptr_t)ebda_seg << 4;

    if (ebda_base >= 0x400 && ebda_base < 0xA0000) {
        for (uintptr_t addr = ebda_base; addr < ebda_base + 1024; addr += 16) {
            const struct acpi_rsdp* r = (const struct acpi_rsdp*)addr;
            if (memcmp(r->signature, "RSD PTR ", 8) == 0 &&
                acpi_checksum_valid(r, 20)) {
                pr_info("acpi: RSDP found in EBDA at 0x%x (rev=%d)\n",
                        (unsigned)addr, r->revision);
                return r;
            }
        }
    }

    /* Method 2: BIOS ROM area */
    for (uintptr_t addr = 0xE0000; addr < 0xFFFFF; addr += 16) {
        const struct acpi_rsdp* r = (const struct acpi_rsdp*)addr;
        if (memcmp(r->signature, "RSD PTR ", 8) == 0 &&
            acpi_checksum_valid(r, 20)) {
            pr_info("acpi: RSDP found in BIOS ROM at 0x%x (rev=%d)\n",
                    (unsigned)addr, r->revision);
            return r;
        }
    }

    return NULL;
}

/* ----- Walk RSDT/XSDT entries -----
 * Calls `callback(header_phys_addr, user_data)` for each entry.
 * Returns the number of entries walked. */
typedef void (*acpi_table_callback)(uintptr_t entry_addr, void* user_data);

struct walk_ctx {
    acpi_table_callback cb;
    void* user_data;
    int count;
};

static void walk_rsdt_xsdt(const struct acpi_rsdp* rsdp, acpi_table_callback cb, void* user_data) {
    if (!rsdp) return;

    const struct acpi_header* root = NULL;
    int entry_size = 4;  /* RSDT default */

    /* Prefer XSDT (64-bit entries) for ACPI 2.0+ */
    if (rsdp->revision >= 2 && rsdp->xsdt_address &&
        phys_ok((uintptr_t)rsdp->xsdt_address)) {
        root = (const struct acpi_header*)(uintptr_t)rsdp->xsdt_address;
        if (memcmp(root->signature, "XSDT", 4) == 0 &&
            acpi_checksum_valid(root, root->length)) {
            entry_size = 8;
            pr_info("acpi: using XSDT at 0x%x (len=%u)\n",
                    (unsigned)rsdp->xsdt_address, root->length);
        } else {
            root = NULL;
        }
    }

    /* Fallback: RSDT (32-bit entries) */
    if (!root && rsdp->rsdt_address && phys_ok(rsdp->rsdt_address)) {
        root = (const struct acpi_header*)(uintptr_t)rsdp->rsdt_address;
        if (memcmp(root->signature, "RSDT", 4) == 0 &&
            acpi_checksum_valid(root, root->length)) {
            entry_size = 4;
            pr_info("acpi: using RSDT at 0x%x (len=%u)\n",
                    (unsigned)rsdp->rsdt_address, root->length);
        } else {
            root = NULL;
        }
    }

    if (!root) {
        pr_warn("acpi: no valid RSDT/XSDT found\n");
        return;
    }

    uint32_t n_entries = (root->length - sizeof(struct acpi_header)) / entry_size;
    pr_info("acpi: walking %u table entries\n", n_entries);

    for (uint32_t i = 0; i < n_entries; i++) {
        uintptr_t entry_addr;
        if (entry_size == 8) {
            const uint64_t* entries = (const uint64_t*)(root + 1);
            entry_addr = (uintptr_t)entries[i];
        } else {
            const uint32_t* entries = (const uint32_t*)(root + 1);
            entry_addr = (uintptr_t)entries[i];
        }

        /* Skip entries outside identity-mapped region */
        if (!phys_ok(entry_addr)) {
            pr_warn("acpi: table entry at 0x%x outside 1GB, skipping\n",
                    (unsigned)entry_addr);
            continue;
        }

        cb(entry_addr, user_data);
    }
}

/* ----- Table-specific parsers ----- */

static void parse_fadt(uintptr_t addr) {
    const struct acpi_header* hdr = (const struct acpi_header*)addr;
    if (memcmp(hdr->signature, "FACP", 4) != 0) return;
    if (!acpi_checksum_valid(hdr, hdr->length)) {
        pr_warn("acpi: FACP checksum invalid\n");
        return;
    }

    const struct acpi_fadt* fadt = (const struct acpi_fadt*)addr;
    g_acpi.fadt_found = 1;
    g_acpi.sci_int = fadt->sci_int;
    g_acpi.pm1a_cnt_blk = fadt->pm1a_cnt_blk;
    g_acpi.pm1a_evt_blk = fadt->pm1a_evt_blk;
    g_acpi.pm1_cnt_len = fadt->pm1_cnt_len;
    g_acpi.fadt_flags = fadt->flags;

    pr_info("acpi: FACP at 0x%x: SCI=%u, PM1a_CNT=0x%x, PM1a_EVT=0x%x, flags=0x%x\n",
            (unsigned)addr, fadt->sci_int, fadt->pm1a_cnt_blk,
            fadt->pm1a_evt_blk, fadt->flags);
}

static void parse_madt(uintptr_t addr) {
    const struct acpi_header* hdr = (const struct acpi_header*)addr;
    if (memcmp(hdr->signature, "APIC", 4) != 0) return;
    if (!acpi_checksum_valid(hdr, hdr->length)) {
        pr_warn("acpi: MADT checksum invalid\n");
        return;
    }

    const struct acpi_madt* madt = (const struct acpi_madt*)addr;
    g_acpi.madt_found = 1;
    g_acpi.lapic_addr = madt->local_apic_addr;

    pr_info("acpi: MADT at 0x%x: LAPIC=0x%x, flags=0x%x\n",
            (unsigned)addr, madt->local_apic_addr, madt->flags);

    /* Walk MADT entries */
    uintptr_t end = addr + hdr->length;
    uintptr_t ptr = addr + sizeof(struct acpi_madt);

    while (ptr + sizeof(struct acpi_madt_entry) <= end) {
        const struct acpi_madt_entry* entry =
            (const struct acpi_madt_entry*)ptr;

        if (entry->length < 2 || ptr + entry->length > end)
            break;

        switch (entry->type) {
        case ACPI_MADT_LAPIC: {
            const struct acpi_madt_lapic* lapic =
                (const struct acpi_madt_lapic*)entry;
            if (lapic->flags & 1) {
                pr_info("acpi:   LAPIC id=%u, ACPI id=%u (enabled)\n",
                        lapic->apic_id, lapic->acpi_id);
            }
            break;
        }
        case ACPI_MADT_IOAPIC: {
            const struct acpi_madt_ioapic* ioapic =
                (const struct acpi_madt_ioapic*)entry;
            g_acpi.ioapic_addr = ioapic->ioapic_addr;
            g_acpi.ioapic_gsi_base = ioapic->gsi_base;
            g_acpi.ioapic_id = ioapic->ioapic_id;
            pr_info("acpi:   IOAPIC id=%u, addr=0x%x, GSI base=%u\n",
                    ioapic->ioapic_id, ioapic->ioapic_addr,
                    ioapic->gsi_base);
            break;
        }
        case ACPI_MADT_ISOVERRIDE: {
            if (g_acpi.n_isa_overrides < ACPI_MAX_ISA_OVERRIDES) {
                const struct acpi_madt_isa_override* iso =
                    (const struct acpi_madt_isa_override*)entry;
                struct acpi_isa_override* o =
                    &g_acpi.isa_overrides[g_acpi.n_isa_overrides];
                o->source_irq = iso->source_irq;
                o->gsi = iso->gsi;
                o->flags = iso->flags;
                g_acpi.n_isa_overrides++;
                pr_info("acpi:   ISA override: IRQ %u -> GSI %u (flags=0x%x)\n",
                        iso->source_irq, iso->gsi, iso->flags);
            }
            break;
        }
        default:
            /* Skip unknown entry types */
            break;
        }

        ptr += entry->length;
    }
}

static void parse_hpet(uintptr_t addr) {
    const struct acpi_header* hdr = (const struct acpi_header*)addr;
    if (memcmp(hdr->signature, "HPET", 4) != 0) return;
    if (!acpi_checksum_valid(hdr, hdr->length)) {
        pr_warn("acpi: HPET checksum invalid\n");
        return;
    }

    const struct acpi_hpet* hpet = (const struct acpi_hpet*)addr;
    g_acpi.hpet_found = 1;
    g_acpi.hpet_base = hpet->base_address.address;
    /* event_timer_block_id encodes: bits 0-7 = rev, bits 8-12 = comparators,
     * bits 13-15 = reserved, bit 16 = counter size, bit 17 = legacy IRQ */
    g_acpi.hpet_comparators = (hpet->event_timer_block_id >> 8) & 0x1F;
    g_acpi.hpet_caps = (hpet->event_timer_block_id >> 16) & 0xFFFF;

    pr_info("acpi: HPET at 0x%llx: %u comparators, caps=0x%x, min_tick=%u\n",
            (unsigned long long)hpet->base_address.address,
            g_acpi.hpet_comparators, g_acpi.hpet_caps,
            hpet->min_tick);
}

/* ----- Dispatch callback for walk_rsdt_xsdt -----
 * Checks signature and routes to the right parser. */
static void acpi_dispatch_entry(uintptr_t addr, void* user_data) {
    (void)user_data;
    if (!phys_ok(addr)) return;

    const struct acpi_header* hdr = (const struct acpi_header*)addr;
    /* Quick length sanity check */
    if (hdr->length < sizeof(struct acpi_header) || hdr->length > 0x100000)
        return;

    if (memcmp(hdr->signature, "FACP", 4) == 0) {
        parse_fadt(addr);
    } else if (memcmp(hdr->signature, "APIC", 4) == 0) {
        parse_madt(addr);
    } else if (memcmp(hdr->signature, "HPET", 4) == 0) {
        parse_hpet(addr);
    }
}

/* ----- Public API ----- */

int acpi_is_initialized(void) {
    return g_acpi.found;
}

int acpi_init(void) {
    /* Idempotency: acpi_init() may be invoked twice during boot
     * (an early call for APIC setup, and the existing late call from
     * kernel_main after battery_init). The second call must NOT memset
     * g_acpi back to zero — that would erase the MADT data the APIC
     * subsystem is already using. */
    if (g_acpi.found) {
        pr_debug("acpi: already initialized — skipping re-discovery\n");
        return 0;
    }

    memset(&g_acpi, 0, sizeof(g_acpi));

    pr_info("acpi: initializing table discovery...\n");

    const struct acpi_rsdp* rsdp = find_rsdp();
    if (!rsdp) {
        pr_warn("acpi: RSDP not found — ACPI unavailable\n");
        return -1;
    }

    g_acpi.found = 1;
    g_acpi.revision = rsdp->revision;
    g_acpi.rsdp_addr = (uintptr_t)rsdp;

    /* Walk all tables, dispatching to parsers */
    walk_rsdt_xsdt(rsdp, acpi_dispatch_entry, NULL);

    /* Summary */
    pr_info("acpi: discovery complete — FACP=%s, MADT=%s, HPET=%s\n",
            g_acpi.fadt_found ? "yes" : "no",
            g_acpi.madt_found ? "yes" : "no",
            g_acpi.hpet_found ? "yes" : "no");
    if (g_acpi.madt_found) {
        pr_info("acpi: %d ISA interrupt overrides cached\n",
                g_acpi.n_isa_overrides);
    }

    return 0;
}

uint32_t acpi_isa_irq_to_gsi(uint8_t isa_irq) {
    for (int i = 0; i < g_acpi.n_isa_overrides; i++) {
        if (g_acpi.isa_overrides[i].source_irq == isa_irq)
            return g_acpi.isa_overrides[i].gsi;
    }
    return (uint32_t)isa_irq;  /* Identity mapping */
}

uint16_t acpi_isa_irq_flags(uint8_t isa_irq) {
    for (int i = 0; i < g_acpi.n_isa_overrides; i++) {
        if (g_acpi.isa_overrides[i].source_irq == isa_irq)
            return g_acpi.isa_overrides[i].flags;
    }
    return 0;  /* No override → ISA default (active-high, edge) */
}
