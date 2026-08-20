/*
 * Lestra OS — NIC Driver Abstraction Layer
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Provides a unified vtable (struct nic_ops) for all NIC drivers.
 * Each driver exports a single 'const struct nic_ops' instance.
 * net.c iterates over registered drivers; first one that initializes wins.
 *
 * KE-14: Eliminates the active_nic integer switch and migrates all
 * drivers to use the shared PCI API (pci_find_device) for discovery.
 */

#ifndef LESTRA_NIC_H
#define LESTRA_NIC_H

#include <lestra/types.h>
#include <lestra/net.h>

/*
 * struct nic_ops — vtable for a NIC driver.
 *
 * All drivers must implement: init, send, recv, get_mac.
 * flush is optional (NULL = no-op). Used by drivers like RTL8139
 * that need post-batch cleanup after draining RX.
 *
 * Buffer semantics are preserved from the pre-refactor API:
 *   send(const void* data, uint16_t len) -> returns len on success, <=0 on error
 *   recv(void* buf, uint16_t bufsz) -> returns packet length, 0 if empty
 */
struct nic_ops {
    const char *name;              /* Driver name, e.g. "virtio_net" */

    /* Probe and initialize the NIC hardware.
     * Returns 1 on success, 0 if device not found or init failed. */
    int  (*init)(void);

    /* Transmit a raw Ethernet frame.
     * Returns frame length on success, 0 or negative on error. */
    int  (*send)(const void *data, uint16_t len);

    /* Receive one packet into the caller's buffer.
     * Returns packet length, 0 if no packet available. */
    int  (*recv)(void *buf, uint16_t bufsz);

    /* Get the MAC address of this NIC. */
    mac_addr_t (*get_mac)(void);

    /* Set the MAC address of this NIC (for MAC randomization).
     * Returns 0 on success, -1 if not supported.
     * May be NULL if the driver doesn't support runtime MAC changes. */
    int  (*set_mac)(mac_addr_t mac);

    /* Optional: flush/cleanup after draining a batch of RX packets.
     * Called once after net_tick() finishes its RX drain loop.
     * May be NULL if the driver doesn't need it. */
    void (*flush)(void);
};

/* Master driver table — populated by each driver's object file.
 * net_init() iterates this array; first init() that returns 1 wins. */
#define NIC_MAX_DRIVERS  8
extern const struct nic_ops *const nic_driver_table[];
extern const int              nic_driver_count;

/* Runtime pointer to the active NIC (set by net_init). */
extern const struct nic_ops *active_nic_ops;

#endif /* LESTRA_NIC_H */
