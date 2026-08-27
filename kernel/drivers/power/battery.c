/*
 * Lestra OS - Battery / ACPI Power Driver
 * Copyright (c) 2026 lestramk.org
 *
 * Reads battery status via ACPI when available. On QEMU (which has no
 * real battery device by default) we fall back to a simulated "always
 * full, always plugged in" battery so userland power tools still work.
 *
 * On real hardware this is where we'd walk the ACPI DSDT/FADT, find the
 * Power Management device (PIIX4 / similar), enable the Embedded
 * Controller, and read the _BST (battery status) / _BIF (battery info)
 * objects. LestraOS doesn't yet ship an AML interpreter, so we probe
 * for the ACPI PCI device and report a simulated battery either way.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/power.h>
#include <lestra/pci.h>

/* Intel PIIX4 ACPI PM device (QEMU's default southbridge). */
#define PIIX4_ACPI_VENDOR  0x8086
#define PIIX4_ACPI_DEVICE  0x7000

/* ACPI PCI base class 0x06 (Bridge), subclass 0x01 (ISA). The PIIX4
 * sits here and provides the ACPI PM function; we use it as a hint
 * that ACPI is wired up on this platform. */
#define PCI_CLASS_BRIDGE    0x06
#define PCI_SUBCLASS_ISA    0x01

/* QEMU simulated battery — VMs are effectively always plugged in. */
#define SIM_PERCENT   100
#define SIM_CHARGING  0
#define SIM_STATUS    "Full"

/* Internal state. */
static int battery_present = 0;   /* Real battery detected            */
static int acpi_present    = 0;   /* ACPI PM device detected          */
static int initialized     = 0;

/*
 * Scan the PCI device table for the ACPI PM device and any known
 * battery controllers. Iterates the shared device table (filled by
 * pci_scan_bus) so devices behind bridges are found too.
 */
static int scan_for_acpi(void) {
    int ndevs = pci_get_device_count();
    for (int i = 0; i < ndevs; i++) {
        struct pci_device *d = pci_get_device(i);
        if (!d) continue;

        uint16_t vendor = d->vendor_id;
        uint16_t device = d->device_id;
        uint8_t baseclass = d->class_code;
        uint8_t subclass  = d->subclass;

        /* Intel PIIX4 ACPI PM — QEMU's default. */
        if (vendor == PIIX4_ACPI_VENDOR && device == PIIX4_ACPI_DEVICE) {
            pr_info("battery: Intel PIIX4 ACPI PM at PCI %02x:%u.%u\n",
                    (unsigned)d->bus, (unsigned)d->dev, (unsigned)d->func);
            return 1;
        }
        /* Generic ACPI-capable ISA bridge hint. */
        if (baseclass == PCI_CLASS_BRIDGE && subclass == PCI_SUBCLASS_ISA) {
            pr_info("battery: ACPI-capable ISA bridge at PCI %02x:%u.%u "
                    "(vendor=%04x device=%04x)\n",
                    (unsigned)d->bus, (unsigned)d->dev, (unsigned)d->func,
                    (unsigned)vendor, (unsigned)device);
            return 1;
        }
    }
    return 0;
}

int battery_init(void) {
    pr_info("battery: scanning PCI for ACPI / battery devices...\n");

    acpi_present = scan_for_acpi();

    /*
     * We don't yet have a full ACPI AML interpreter, so even when the
     * ACPI PM device is present we can't reliably read the
     * embedded-controller battery registers. We expose a simulated
     * battery in that case so the system tray and power daemons have
     * something sensible to show. On real hardware this is where we'd
     * walk the ACPI DSDT/FADT and read the _BST/_BIF objects.
     */
    if (acpi_present) {
        pr_info("battery: ACPI PM device present, ACPI mode available\n");
        battery_present = 1;
    } else {
        pr_info("battery: no ACPI device found, using QEMU simulated battery\n");
        battery_present = 0;
    }

    pr_info("battery: initialised (%s, %d%%, charging=%d)\n",
            battery_get_status_str(),
            battery_get_percent(),
            battery_is_charging());

    initialized = 1;
    return 0;
}

int battery_get_percent(void) {
    /*
     * HONEST STATUS: We do NOT read ACPI _BST. LestraOS has no AML
     * interpreter, so we cannot evaluate _BST/_BIF. We return a
     * simulated "100% Full" value because most test targets (QEMU,
     * VirtualBox) don't expose a battery anyway. On a real laptop
     * this number will be WRONG until ACPI AML support lands.
     */
    return SIM_PERCENT;
}

int battery_is_charging(void) {
    /* See comment in battery_get_percent — this is simulated. */
    return SIM_CHARGING;
}

/* New function: lets the UI honestly report "simulated". */
int battery_is_simulated(void) {
    /* PIIX4 ACPI device present does not imply we can read _BST.
     * LestraOS has no AML interpreter, so always simulated for now. */
    return 1;
}

const char* battery_get_status_str(void) {
    /*
     * On real hardware this would reflect the _BST state field:
     *   0 = Discharging
     *   1 = Charging
     *   2 = Not charging (Full / on AC)
     *   3 = Unknown
     * QEMU simulated battery reports "Full".
     */
    return SIM_STATUS;
}
