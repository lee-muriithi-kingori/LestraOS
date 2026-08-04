/*
 * Lestra OS — NIC Driver Table
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Central registry of all NIC driver vtables.
 * Each driver object file exports a 'const struct nic_ops' symbol;
 * we reference them here in priority order.
 *
 * KE-14: Replaces the active_nic integer switch in net.c.
 */

#include <lestra/nic.h>

/* Driver vtables — declared in each driver's .c file */
extern const struct nic_ops virtio_net_ops;
extern const struct nic_ops e1000_ops;
extern const struct nic_ops rtl8139_ops;

/* Priority order: VirtIO (KVM/QEMU VPS) > E1000 (Intel) > RTL8139 (real hw) */
const struct nic_ops *const nic_driver_table[] = {
    &virtio_net_ops,
    &e1000_ops,
    &rtl8139_ops,
    NULL
};
const int nic_driver_count = 3;

/* Runtime: points to the NIC that won the init race */
const struct nic_ops *active_nic_ops = NULL;
