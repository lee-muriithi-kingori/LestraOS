/*
 * Lestra OS - ACPI Table Discovery Subsystem
 * Copyright (c) 2026 lestramk.org
 *
 * Discovers and parses ACPI tables (RSDP → RSDT/XSDT → MADT/HPET/FACP).
 * Provides a cached database of hardware topology for drivers:
 *   - IOAPIC base address and GSI range
 *   - Local APIC base address
 *   - ISA interrupt override mappings
 *   - HPET base address and capabilities
 *   - FACP (FADT) PM1a/PM1b control/event block addresses
 *   - SCI interrupt line
 *
 * IMPORTANT: The first 1GB of physical memory is identity-mapped via
 * 2MB huge pages (boot.asm), so all ACPI table physical addresses
 * in [0, 1GB) can be dereferenced directly without ioremap.
 *
 * No AML interpreter. No ACPI power management. Pure discovery.
 */

#ifndef LESTRA_ACPI_H
#define LESTRA_ACPI_H

#include <lestra/types.h>

/* Maximum ISA interrupt overrides we track */
#define ACPI_MAX_ISA_OVERRIDES  16

/* ----- ACPI table structures (physical memory layout) ----- */

struct acpi_rsdp {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;          /* 0=ACPI 1.0 (RSDT), 2+=ACPI 2.0+ (XSDT) */
    uint32_t rsdt_address;      /* Physical address of RSDT */
    uint32_t length;            /* XSDT length (ACPI 2.0+) */
    uint64_t xsdt_address;      /* Physical address of XSDT (ACPI 2.0+) */
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __packed;

struct acpi_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    char     creator_id[4];
    uint32_t creator_revision;
} __packed;

/* ----- FACP / FADT ----- */

struct acpi_fadt {
    struct acpi_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  preferred_pm_profile;
    uint8_t  reserved1;
    uint16_t sci_int;            /* SCI interrupt vector */
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;       /* PM1a Control Block */
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alrm;
    uint8_t  mon_alrm;
    uint8_t  century;
    uint16_t iapc_boot_arch;
    uint8_t  reserved2;
    uint32_t flags;
    uint32_t reset_reg[2];      /* Generic address */
    uint8_t  reset_value;
    uint8_t  reserved3[3];
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    uint64_t x_pm1a_evt_blk;
    uint64_t x_pm1b_evt_blk;
    uint64_t x_pm1a_cnt_blk;
    uint64_t x_pm1b_cnt_blk;
    uint64_t x_pm2_cnt_blk;
    uint64_t x_pm_tmr_blk;
    uint64_t x_gpe0_blk;
    uint64_t x_gpe1_blk;
} __packed;

/* ----- MADT (Multiple APIC Description Table) ----- */

struct acpi_madt {
    struct acpi_header header;
    uint32_t local_apic_addr;   /* Local APIC base address */
    uint32_t flags;              /* PCAT_COMPAT bit 0 */
    /* Followed by interrupt controller entries */
} __packed;

/* MADT entry header */
struct acpi_madt_entry {
    uint8_t  type;
    uint8_t  length;
} __packed;

/* MADT entry types */
#define ACPI_MADT_LAPIC          0
#define ACPI_MADT_IOAPIC         1
#define ACPI_MADT_ISOVERRIDE     2
#define ACPI_MADT_NMI            3
#define ACPI_MADT_LAPIC_NMI      4
#define ACPI_MADT_LAPIC_OVERRIDE 5
#define ACPI_MADT_IOSAPIC        6
#define ACPI_MADT_LSAPIC         7
#define ACPI_MADT_PLATFORM_SRC   8
#define ACPI_MADT_LX2APIC        9
#define ACPI_MADT_LX2APIC_NMI    10

/* MADT: Local APIC */
struct acpi_madt_lapic {
    uint8_t  type;               /* ACPI_MADT_LAPIC */
    uint8_t  length;             /* 8 */
    uint8_t  acpi_id;
    uint8_t  apic_id;
    uint32_t flags;              /* 1 = processor is enabled */
} __packed;

/* MADT: IOAPIC */
struct acpi_madt_ioapic {
    uint8_t  type;               /* ACPI_MADT_IOAPIC */
    uint8_t  length;             /* 12 */
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;        /* MMIO base address */
    uint32_t gsi_base;           /* Global System Interrupt base */
} __packed;

/* MADT: ISA Interrupt Source Override */
struct acpi_madt_isa_override {
    uint8_t  type;               /* ACPI_MADT_ISOVERRIDE */
    uint8_t  length;             /* 10 */
    uint8_t  bus;                /* 0 = ISA */
    uint8_t  source_irq;         /* ISA IRQ (0-15) */
    uint32_t gsi;                /* Global System Interrupt */
    uint16_t flags;              /* Polarity/trigger mode */
} __packed;

/* ----- ACPI Generic Address Structure (GAS) -----
 * Used in HPET, FADT extended fields, etc.
 * 8 bytes: address_space_id, register_bit_width, register_bit_offset,
 *          access_size, address (64-bit LE). */
struct acpi_gas {
    uint8_t  address_space_id;   /* 0=memory, 1=I/O, 2=PCI */
    uint8_t  register_bit_width;
    uint8_t  register_bit_offset;
    uint8_t  access_size;
    uint64_t address;             /* 64-bit address */
} __packed;

/* ----- HPET (High Precision Event Timer) ----- */

struct acpi_hpet {
    struct acpi_header header;       /* 36 bytes */
    uint32_t event_timer_block_id;   /* Hardware rev/cap info */
    struct acpi_gas base_address;    /* Event Timer Block address (12 bytes) */
    uint8_t  hpet_number;            /* ACPI HPET number */
    uint16_t min_tick;                /* Minimum tick */
    uint8_t  page_protection;         /* Page protection */
} __packed;  /* 36+4+12+1+2+1 = 56 bytes */

/* ----- Cached ACPI database (populated by acpi_init) ----- */

struct acpi_isa_override {
    uint8_t  source_irq;         /* Original ISA IRQ */
    uint32_t gsi;                /* Mapped GSI */
    uint16_t flags;              /* Polarity/trigger */
};

struct acpi_info {
    /* Discovery status */
    int     found;
    int     revision;            /* RSDP revision (0=1.0, 2+=2.0+) */

    /* RSDP */
    uintptr_t rsdp_addr;

    /* FACP / FADT */
    int     fadt_found;
    uint16_t sci_int;            /* SCI interrupt line */
    uint32_t pm1a_cnt_blk;       /* PM1a Control Block I/O port */
    uint32_t pm1a_evt_blk;       /* PM1a Event Block I/O port */
    uint8_t  pm1_cnt_len;        /* Width of PM1_CNT register */
    uint32_t fadt_flags;

    /* MADT */
    int     madt_found;
    uint32_t lapic_addr;         /* Local APIC MMIO base */
    uint32_t ioapic_addr;        /* IOAPIC MMIO base */
    uint32_t ioapic_gsi_base;    /* IOAPIC GSI base */
    int     ioapic_id;
    int     n_isa_overrides;
    struct acpi_isa_override isa_overrides[ACPI_MAX_ISA_OVERRIDES];

    /* HPET */
    int     hpet_found;
    uint64_t hpet_base;          /* HPET MMIO base address */
    uint8_t  hpet_comparators;   /* Number of comparators */
    uint16_t hpet_caps;
};

/* ----- API ----- */

/* Initialize ACPI table discovery.
 * Safe to call anytime after serial/framebuffer init.
 * Non-fatal: if tables not found, acpi_info.found == 0.
 * Returns 0 on success, -1 if RSDP not found. */
int acpi_init(void);

/* Global ACPI database (read-only after acpi_init) */
extern struct acpi_info g_acpi;

/* Map an ISA IRQ to its GSI using cached overrides.
 * If no override exists, returns the IRQ itself (identity). */
uint32_t acpi_isa_irq_to_gsi(uint8_t isa_irq);

/* ISA interrupt-source override flags (MADT IntSrcOverride entry).
 * Returns 0 if no override exists for this IRQ (meaning the ISA default:
 * active-high, edge-triggered). Otherwise returns the raw 16-bit flags:
 *   bits 0-1: polarity (0=conforming, 1=high, 2=reserved, 3=low)
 *   bits 2-3: trigger  (0=conforming, 1=edge, 2=reserved, 3=level)
 * "Conforming" for ISA means active-high edge, which the caller should
 * treat identically to flags==0. */
uint16_t acpi_isa_irq_flags(uint8_t isa_irq);

/* Idempotency guard: acpi_init() may be called more than once during
 * boot (early for APIC setup, and again later from the existing init
 * sequence). The second call is a no-op so g_acpi is never reset. */
int acpi_is_initialized(void);

#endif /* LESTRA_ACPI_H */
