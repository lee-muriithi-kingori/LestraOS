/*
 * Lestra OS - MAC Address Changer
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Randomizes the NIC's MAC address every 10 minutes using the
 * kernel CSPRNG. Improves privacy by preventing long-term MAC
 * tracking across network connections.
 *
 * The MAC is changed by calling net_set_mac() which updates both
 * the hardware NIC driver and the software stack copy. The first
 * octet has bit 1 (locally administered) set and bit 0 (multicast)
 * cleared per IEEE 802 standard.
 */

#include <lestra/types.h>
#include <lestra/net.h>
#include <lestra/printk.h>
#include <lestra/timer.h>

/* Randomize interval: 10 minutes in milliseconds */
#define MAC_RANDOMIZE_INTERVAL_MS  (10ULL * 60 * 1000)

/* State */
static uint64_t last_randomize_ms = 0;
static int      mac_changer_initialized = 0;

/* Get 6 random bytes for a new MAC address.
 * Sets bit 1 (locally administered) and clears bit 0 (unicast). */
static mac_addr_t generate_random_mac(void) {
    mac_addr_t mac;
    get_random_bytes(mac.bytes, 6);

    /* Set locally administered bit, clear multicast bit */
    mac.bytes[0] = (mac.bytes[0] | 0x02) & 0xFE;

    return mac;
}

/* Initialize the MAC changer. Called once at boot after net_init(). */
void mac_changer_init(void) {
    last_randomize_ms = timer_get_ms();
    mac_changer_initialized = 1;
    pr_info("mac_changer: initialized (interval %llu min)\n",
            (unsigned long long)(MAC_RANDOMIZE_INTERVAL_MS / 60000));
}

/* Called periodically from the main loop or timer tick.
 * Checks if it's time to rotate the MAC address. */
void mac_changer_tick(void) {
    if (!mac_changer_initialized) return;

    uint64_t now = timer_get_ms();
    if (now - last_randomize_ms < MAC_RANDOMIZE_INTERVAL_MS) return;

    mac_addr_t old_mac = net_get_mac();
    mac_addr_t new_mac = generate_random_mac();

    if (net_set_mac(new_mac) == 0) {
        pr_info("mac_changer: MAC changed %02x:%02x:%02x:%02x:%02x:%02x"
                " -> %02x:%02x:%02x:%02x:%02x:%02x\n",
                old_mac.bytes[0], old_mac.bytes[1], old_mac.bytes[2],
                old_mac.bytes[3], old_mac.bytes[4], old_mac.bytes[5],
                new_mac.bytes[0], new_mac.bytes[1], new_mac.bytes[2],
                new_mac.bytes[3], new_mac.bytes[4], new_mac.bytes[5]);
    } else {
        pr_warn("mac_changer: failed to set new MAC (driver may not support it)\n");
    }

    last_randomize_ms = now;
}

/* Force an immediate MAC randomization (for testing or manual trigger) */
void mac_changer_randomize_now(void) {
    last_randomize_ms = 0;
    mac_changer_tick();
}
