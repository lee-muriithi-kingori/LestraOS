/*
 * Lestra OS - WiFi / WLAN Interface Framework
 * Copyright (c) 2026 lestramk.org
 *
 * LestraOS does not yet ship a real WiFi driver (no 802.11 MAC layer
 * in tree). This module provides the userland-facing WiFi API on top
 * of a small simulated network list so the settings UI, network
 * picker, and tray applet all have something to render.
 *
 * When a real WiFi MAC driver (e.g. for an Atheros ath9k or Intel
 * iwlwifi card) lands in tree, only the scan/connect bodies below
 * need to change — the public API stays the same.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/wifi.h>
#include <string.h>

#define WIFI_MAX_SCAN  16
#define WIFI_SSID_LEN  33   /* 802.11 max SSID (32) + NUL            */

typedef struct {
    char ssid[WIFI_SSID_LEN];
    int  signal;            /* 0-100, higher is better              */
} wifi_network_t;

static wifi_network_t scan_results[WIFI_MAX_SCAN];
static int  scan_count    = 0;
static int  connected     = 0;
static int  connected_idx = -1;   /* index into scan_results, or -1  */
static char connected_ssid[WIFI_SSID_LEN];
static int  initialized = 0;

/* Built-in simulated networks. Real hardware would replace this with
 * a scan request to the WiFi MAC driver and a proper beacon parse. */
static const wifi_network_t sim_networks[] = {
    { "LestraNet",  95 },
    { "HomeWiFi",   67 },
    { "CoffeeShop", 34 },
};
#define SIM_NET_COUNT  ((int)(sizeof(sim_networks) / sizeof(sim_networks[0])))

void wifi_init(void) {
    pr_info("wifi: initialising WLAN framework (simulated)\n");
    memset(scan_results, 0, sizeof(scan_results));
    memset(connected_ssid, 0, sizeof(connected_ssid));
    scan_count    = 0;
    connected     = 0;
    connected_idx = -1;
    initialized   = 1;
    pr_info("wifi: ready\n");
}

int wifi_scan(void) {
    if (!initialized) return 0;

    pr_info("wifi: scanning for networks...\n");
    memset(scan_results, 0, sizeof(scan_results));
    scan_count = 0;

    /* Populate from the simulated network table. */
    for (int i = 0; i < SIM_NET_COUNT && scan_count < WIFI_MAX_SCAN; i++) {
        strcpy(scan_results[scan_count].ssid, sim_networks[i].ssid);
        scan_results[scan_count].signal = sim_networks[i].signal;
        scan_count++;
    }

    pr_info("wifi: scan complete, %d network(s) found\n", scan_count);
    for (int i = 0; i < scan_count; i++) {
        pr_info("  [%d] %s signal=%d%%\n",
                i, scan_results[i].ssid, scan_results[i].signal);
    }
    return scan_count;
}

const char* wifi_get_ssid(int idx) {
    if (!initialized || idx < 0 || idx >= scan_count) {
        return NULL;
    }
    return scan_results[idx].ssid;
}

int wifi_get_signal(int idx) {
    if (!initialized || idx < 0 || idx >= scan_count) {
        return -1;
    }
    return scan_results[idx].signal;
}

int wifi_connect(const char* ssid, const char* password) {
    if (!initialized || !ssid) return -1;

    pr_info("wifi: connect request to \"%s\"\n", ssid);

    /* Look in scan results first. If no scan was done, fall back to
     * the built-in simulated network list so userland can connect
     * without an explicit scan call. */
    int found  = -1;
    int signal = -1;
    for (int i = 0; i < scan_count; i++) {
        if (strcmp(scan_results[i].ssid, ssid) == 0) {
            found  = i;
            signal = scan_results[i].signal;
            break;
        }
    }
    if (found < 0) {
        for (int i = 0; i < SIM_NET_COUNT; i++) {
            if (strcmp(sim_networks[i].ssid, ssid) == 0) {
                found  = i;
                signal = sim_networks[i].signal;
                break;
            }
        }
    }
    if (found < 0) {
        pr_warn("wifi: SSID \"%s\" not found\n", ssid);
        return -1;
    }

    /* Simulated auth: any password (including empty) succeeds for the
     * demo networks. Real hardware would do a WPA2/3 handshake here. */
    (void)password;

    connected     = 1;
    connected_idx = found;
    strncpy(connected_ssid, ssid, WIFI_SSID_LEN - 1);
    connected_ssid[WIFI_SSID_LEN - 1] = '\0';

    pr_info("wifi: connected to \"%s\" (signal %d%%)\n",
            connected_ssid, signal);
    return 0;
}

int wifi_disconnect(void) {
    if (!initialized) return -1;
    if (!connected) {
        pr_info("wifi: not connected, nothing to disconnect\n");
        return 0;
    }
    pr_info("wifi: disconnecting from \"%s\"\n", connected_ssid);
    connected     = 0;
    connected_idx = -1;
    memset(connected_ssid, 0, sizeof(connected_ssid));
    return 0;
}

int wifi_is_connected(void) {
    return connected ? 1 : 0;
}

const char* wifi_get_connected_ssid(void) {
    if (!connected) return NULL;
    return connected_ssid;
}
