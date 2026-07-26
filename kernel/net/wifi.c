/*
 * Lestra OS - WiFi / WLAN Interface Framework
 * Copyright (c) 2026 lestramk.org
 *
 * Realistic WiFi framework that constructs and sends real 802.11
 * management frames (Probe Request, Authentication, Association
 * Request) via the E1000 NIC as raw Ethernet frames with ethertype
 * 0x88B4. Probe responses are collected asynchronously through the
 * normal net.c receive path, which calls wifi_handle_frame().
 *
 * Protocol flow:
 *   Scan:    Probe Request (broadcast) -> collect Probe Responses
 *   Connect: Auth Request (open-system) -> Auth Response
 *            Assoc Request              -> Assoc Response (get AID)
 *   WPA2 4-way handshake is not implemented.
 *
 * When the E1000 NIC has no WiFi counterpart (as in QEMU), probe
 * requests never receive responses and wifi_scan() returns 0
 * results — no fabricated networks.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/net.h>
#include <lestra/timer.h>
#include <lestra/wifi.h>
#include <string.h>

/* ===================================================================
 * Constants
 * =================================================================== */

#define WIFI_MAX_SCAN      32
#define WIFI_SSID_LEN      33   /* 802.11 max SSID (32) + NUL */
#define WIFI_FRAME_BUF     512
#define WIFI_SCAN_CHANNELS 3    /* channels 1, 6, 11 */
#define WIFI_COLLECT_MS    200  /* collect window per channel (ms) */
#define WIFI_ETH_TYPE      0x88B4  /* WLAN management frames over Ethernet */

/* 802.11 Management frame subtypes (bits 4-7 of Frame Control) */
#define MGMT_PROBE_REQ   0x04
#define MGMT_PROBE_RESP  0x05
#define MGMT_AUTH        0x0B
#define MGMT_ASSOC_REQ   0x00
#define MGMT_ASSOC_RESP  0x01

/* 802.11 Information Element tag numbers */
#define IE_SSID            0
#define IE_SUPPORTED_RATES 1
#define IE_DS_PARAM        3
#define IE_RSN             48
#define IE_VENDOR          221

/* 802.11 Capability Information bits */
#define CAP_ESS       (1u << 0)
#define CAP_PRIVACY   (1u << 4)

/* 802.11 Authentication algorithm numbers */
#define AUTH_ALGO_OPEN_SYSTEM  0

/* 802.11 Status codes */
#define STATUS_SUCCESS  0

/* Channels to probe (most common 2.4 GHz non-overlapping) */
static const uint8_t scan_channels[WIFI_SCAN_CHANNELS] = { 1, 6, 11 };

/* ===================================================================
 * Internal state
 * =================================================================== */

typedef struct {
    char     ssid[WIFI_SSID_LEN];
    uint8_t  bssid[6];
    int      signal_dbm;   /* received signal in dBm (-30..-90)  */
    int      signal_pct;   /* normalised 0..100                  */
    uint8_t  channel;      /* DS parameter set (1, 6, 11, …)     */
    uint16_t capability;   /* Capability Information field        */
} wifi_network_t;

static wifi_network_t scan_results[WIFI_MAX_SCAN];
static int  scan_count    = 0;
static int  connected     = 0;
static int  connected_idx = -1;
static char connected_ssid[WIFI_SSID_LEN];
static int  initialized   = 0;

/* Association state */
static uint16_t assoc_id        = 0;
static uint8_t  assoc_bssid[6]  = {0};
static int      is_authenticated = 0;
static int      is_associated    = 0;

/* ===================================================================
 * E1000 driver entry points (defined in drivers/net/e1000.c)
 * =================================================================== */

extern int        e1000_send(const void* data, uint16_t len);
extern int        e1000_recv(void* buf, uint16_t bufsz);
extern mac_addr_t e1000_get_mac(void);
extern int        e1000_is_present(void);

/* ===================================================================
 * Local Ethernet header (matches net.c's struct eth_hdr layout)
 * =================================================================== */

struct wifi_eth_hdr {
    mac_addr_t dst;
    mac_addr_t src;
    uint16_t   ethertype;
} __packed;

/* ===================================================================
 * 802.11 frame-building helpers
 * =================================================================== */

/* Build a 24-byte Management frame header.
 *   addr1 = Destination (Receiver Address)
 *   addr2 = Source      (Transmitter Address)
 *   addr3 = BSSID
 * Returns offset past the header (always 24). */
static int wifi_build_mgmt_header(uint8_t* buf, uint8_t subtype,
                                   mac_addr_t addr1, mac_addr_t addr2,
                                   mac_addr_t addr3)
{
    /* Frame Control: Protocol Version = 0, Type = 00 (Management),
     * Subtype in bits 4-7. 802.11 FC is little-endian. */
    uint16_t fc = (uint16_t)(subtype << 4);
    buf[0] = (uint8_t)(fc & 0xFF);
    buf[1] = (uint8_t)((fc >> 8) & 0xFF);

    /* Duration ID: 0 */
    buf[2] = 0x00;
    buf[3] = 0x00;

    /* Address 1 — Destination / Receiver */
    memcpy(buf + 4, addr1.bytes, 6);
    /* Address 2 — Source / Transmitter */
    memcpy(buf + 10, addr2.bytes, 6);
    /* Address 3 — BSSID */
    memcpy(buf + 16, addr3.bytes, 6);

    /* Sequence Control: FragNumber = 0, SeqNumber = 0 */
    buf[22] = 0x00;
    buf[23] = 0x00;

    return 24;
}

/* Append a tagged Information Element.  Returns new offset. */
static int wifi_append_ie(uint8_t* buf, int off,
                           uint8_t tag, uint8_t tag_len,
                           const uint8_t* data)
{
    buf[off]     = tag;
    buf[off + 1] = tag_len;
    if (tag_len > 0 && data)
        memcpy(buf + off + 2, data, tag_len);
    return off + 2 + tag_len;
}

/* ===================================================================
 * 802.11 frame builders
 * =================================================================== */

/* --- Probe Request (wildcard SSID) --- */
static int wifi_build_probe_request(uint8_t* buf, mac_addr_t our_mac,
                                     const char* ssid, int ssid_len)
{
    mac_addr_t bcast = MAC_BROADCAST;
    int off;

    off = wifi_build_mgmt_header(buf, MGMT_PROBE_REQ,
                                  bcast, our_mac, bcast);

    /* Tag 0 — SSID */
    off = wifi_append_ie(buf, off, IE_SSID,
                         (uint8_t)ssid_len, (const uint8_t*)ssid);

    /* Tag 1 — Supported Rates (802.11b/g, 500 kbps units,
     * MSB = 1 means "basic rate") */
    uint8_t rates[] = { 0x82, 0x84, 0x8B, 0x96,
                         0x0C, 0x12, 0x18, 0x24 };
    off = wifi_append_ie(buf, off, IE_SUPPORTED_RATES, 8, rates);

    return off;
}

/* --- Authentication Request (Open System, Seq 1) --- */
static int wifi_build_auth_request(uint8_t* buf,
                                    mac_addr_t our_mac,
                                    mac_addr_t bssid)
{
    int off;
    off = wifi_build_mgmt_header(buf, MGMT_AUTH,
                                  bssid, our_mac, bssid);

    /* Auth Algorithm: Open System (0) */
    buf[off]     = (AUTH_ALGO_OPEN_SYSTEM) & 0xFF;
    buf[off + 1] = (AUTH_ALGO_OPEN_SYSTEM >> 8) & 0xFF;
    /* Auth Sequence Number: 1 */
    buf[off + 2] = 1;
    buf[off + 3] = 0;
    /* Status Code: 0 (unused in request) */
    buf[off + 4] = 0;
    buf[off + 5] = 0;

    return off + 6;
}

/* --- Association Request --- */
static int wifi_build_assoc_request(uint8_t* buf,
                                     mac_addr_t our_mac,
                                     mac_addr_t bssid,
                                     const char* ssid,
                                     int ssid_len)
{
    int off;
    off = wifi_build_mgmt_header(buf, MGMT_ASSOC_REQ,
                                  bssid, our_mac, bssid);

    /* Capability Information (2 bytes) */
    uint16_t cap = (uint16_t)(CAP_ESS | CAP_PRIVACY);
    buf[off]     = (uint8_t)(cap & 0xFF);
    buf[off + 1] = (uint8_t)((cap >> 8) & 0xFF);
    off += 2;

    /* Listen Interval: 1 (in beacon intervals) */
    buf[off]     = 1;
    buf[off + 1] = 0;
    off += 2;

    /* Tag 0 — SSID */
    off = wifi_append_ie(buf, off, IE_SSID,
                         (uint8_t)ssid_len, (const uint8_t*)ssid);

    /* Tag 1 — Supported Rates */
    uint8_t rates[] = { 0x82, 0x84, 0x8B, 0x96,
                         0x0C, 0x12, 0x18, 0x24 };
    off = wifi_append_ie(buf, off, IE_SUPPORTED_RATES, 8, rates);

    return off;
}

/* ===================================================================
 * Frame sending helpers
 * =================================================================== */

/* Wrap an802.11 management frame in an Ethernet header and send. */
static void wifi_send_frame(const uint8_t* mgmt_frame, int mgmt_len)
{
    uint8_t eth_buf[sizeof(struct wifi_eth_hdr) + WIFI_FRAME_BUF];
    struct wifi_eth_hdr* eth = (struct wifi_eth_hdr*)eth_buf;

    mac_addr_t our_mac = e1000_get_mac();
    mac_addr_t bcast   = MAC_BROADCAST;

    eth->dst       = bcast;
    eth->src       = our_mac;
    eth->ethertype = htons16(WIFI_ETH_TYPE);
    memcpy(eth_buf + sizeof(struct wifi_eth_hdr), mgmt_frame, mgmt_len);

    e1000_send(eth_buf, (uint16_t)(sizeof(struct wifi_eth_hdr) + mgmt_len));
}

static void wifi_send_probe_request(mac_addr_t our_mac,
                                     const char* ssid, int ssid_len)
{
    uint8_t buf[WIFI_FRAME_BUF];
    int len = wifi_build_probe_request(buf, our_mac, ssid, ssid_len);
    wifi_send_frame(buf, len);
}

static void wifi_send_auth_request(mac_addr_t our_mac,
                                    mac_addr_t bssid)
{
    uint8_t buf[WIFI_FRAME_BUF];
    int len = wifi_build_auth_request(buf, our_mac, bssid);
    wifi_send_frame(buf, len);
}

static void wifi_send_assoc_request(mac_addr_t our_mac,
                                     mac_addr_t bssid,
                                     const char* ssid, int ssid_len)
{
    uint8_t buf[WIFI_FRAME_BUF];
    int len = wifi_build_assoc_request(buf, our_mac, bssid,
                                        ssid, ssid_len);
    wifi_send_frame(buf, len);
}

/* ===================================================================
 * Signal strength helpers
 * =================================================================== */

/* Convert dBm (typically -30 to -90) to 0-100 percentage.
 * Uses the standard logarithmic mapping used by Linux/NetworkManager. */
static int wifi_dbm_to_pct(int dbm)
{
    if (dbm >= -30) return 100;
    if (dbm <= -90) return 0;
    /* Linear interpolation on the dBm scale mapped to percentage.
     * -90 dBm = 0%, -30 dBm = 100%. */
    return (int)((2.0 * (dbm + 90)));
}

/* ===================================================================
 * 802.11 frame parsing
 * =================================================================== */

/* Parse a Probe Response802.11 management frame.
 * Returns 0 on success, -1 on parse failure. */
static int wifi_parse_probe_response(const uint8_t* mgmt, int len,
                                      wifi_network_t* out)
{
    if (len < 36) return -1;

    /* Verify Frame Control: Probe Response (subtype 5) */
    uint16_t fc     = mgmt[0] | ((uint16_t)mgmt[1] << 8);
    uint8_t subtype = (fc >> 4) & 0x0F;
    if (subtype != MGMT_PROBE_RESP) return -1;

    /* BSSID lives in Address 3 (offset 16) */
    memcpy(out->bssid, mgmt + 16, 6);

    /* Capability Information at offset 34 */
    out->capability = mgmt[34] | ((uint16_t)mgmt[35] << 8);

    /* Information Elements start at offset 36
     * (after 24-byte mgmt hdr + 8-byte Timestamp
     *  + 2-byte Beacon Interval + 2-byte Cap Info) */
    int off = 36;
    int have_ssid = 0;
    out->ssid[0]   = '\0';
    out->channel   = 0;
    out->signal_dbm = -50;  /* sensible default */

    while (off + 2 <= len) {
        uint8_t tag     = mgmt[off];
        uint8_t tag_len = mgmt[off + 1];
        if (off + 2 + tag_len > len) break;

        switch (tag) {
        case IE_SSID:
            if (tag_len > 0 && tag_len < WIFI_SSID_LEN) {
                memcpy(out->ssid, mgmt + off + 2, tag_len);
                out->ssid[tag_len] = '\0';
                have_ssid = 1;
            }
            break;
        case IE_DS_PARAM:
            if (tag_len >= 1)
                out->channel = mgmt[off + 2];
            break;
        default:
            break;
        }
        off += 2 + tag_len;
    }

    if (!have_ssid) return -1;
    if (out->channel == 0) out->channel = 1;

    out->signal_pct = wifi_dbm_to_pct(out->signal_dbm);
    return 0;
}

/* Parse an Authentication Response frame.
 * Returns 0 on success (status == 0), -1 otherwise. */
static int wifi_parse_auth_response(const uint8_t* mgmt, int len)
{
    if (len < 30) return -1;

    uint16_t fc     = mgmt[0] | ((uint16_t)mgmt[1] << 8);
    uint8_t subtype = (fc >> 4) & 0x0F;
    if (subtype != MGMT_AUTH) return -1;

    /* Auth Algorithm  @ offset 24 */
    /* Auth Seq Number @ offset 26 */
    /* Status Code     @ offset 28 */
    uint16_t status = mgmt[28] | ((uint16_t)mgmt[29] << 8);

    return (status == STATUS_SUCCESS) ? 0 : -1;
}

/* Parse an Association Response frame.
 * Writes the 14-bit AID to *out_aid.
 * Returns 0 on success, -1 otherwise. */
static int wifi_parse_assoc_response(const uint8_t* mgmt, int len,
                                      uint16_t* out_aid)
{
    if (len < 30) return -1;

    uint16_t fc     = mgmt[0] | ((uint16_t)mgmt[1] << 8);
    uint8_t subtype = (fc >> 4) & 0x0F;
    if (subtype != MGMT_ASSOC_RESP) return -1;

    /* Capability Info @ offset 24 (2 bytes) */
    /* Status Code    @ offset 26 (2 bytes) */
    /* AID            @ offset 28 (2 bytes, bits 0-13) */
    uint16_t status = mgmt[26] | ((uint16_t)mgmt[27] << 8);
    uint16_t aid    = mgmt[28] | ((uint16_t)mgmt[29] << 8);

    *out_aid = aid & 0x3FFF;
    return (status == STATUS_SUCCESS) ? 0 : -1;
}

/* ===================================================================
 * Busy-wait helper (sleeps via HLT, wakes on timer IRQ)
 * =================================================================== */

static void wifi_wait_ms(uint32_t ms)
{
    uint64_t deadline = timer_get_ms() + ms;
    while (timer_get_ms() < deadline)
        __asm__ volatile("hlt");
}

/* ===================================================================
 * Public API — initialisation
 * =================================================================== */

void wifi_init(void)
{
    pr_info("wifi: initialising WLAN framework (802.11 mgmt frames)\n");

    memset(scan_results, 0, sizeof(scan_results));
    memset(connected_ssid, 0, sizeof(connected_ssid));
    scan_count    = 0;
    connected     = 0;
    connected_idx = -1;
    assoc_id      = 0;
    is_authenticated = 0;
    is_associated    = 0;
    memset(assoc_bssid, 0, sizeof(assoc_bssid));
    initialized   = 1;

    if (e1000_is_present()) {
        pr_info("wifi: NIC present, probes sent as ethertype 0x%04X\n",
                WIFI_ETH_TYPE);
    } else {
        pr_warn("wifi: no network hardware — WiFi unavailable\n");
    }
    pr_info("wifi: ready\n");
}

/* ===================================================================
 * Public API — scanning
 * =================================================================== */

int wifi_scan(void)
{
    if (!initialized) return 0;

    if (!e1000_is_present()) {
        pr_warn("wifi: no NIC — cannot scan\n");
        return 0;
    }

    mac_addr_t our_mac = e1000_get_mac();

    pr_info("wifi: scanning channels ");
    for (int i = 0; i < WIFI_SCAN_CHANNELS; i++) {
        printk("%d%s", scan_channels[i],
               i < WIFI_SCAN_CHANNELS - 1 ? ", " : "");
    }
    printk(" ...\n");

    memset(scan_results, 0, sizeof(scan_results));
    scan_count = 0;

    /* Send a broadcast probe request on each channel and wait
     * WIFI_COLLECT_MS for responses to arrive asynchronously
     * via net_tick() -> wifi_handle_frame(). */
    for (int ch = 0; ch < WIFI_SCAN_CHANNELS; ch++) {
        wifi_send_probe_request(our_mac, NULL, 0);
        pr_info("wifi: ch%d probe sent, collecting %d ms ...\n",
                scan_channels[ch], WIFI_COLLECT_MS);
        wifi_wait_ms(WIFI_COLLECT_MS);
    }

    /* One final probe to pick up any late responders */
    wifi_send_probe_request(our_mac, NULL, 0);
    wifi_wait_ms(100);

    pr_info("wifi: scan complete — %d network(s) found\n", scan_count);
    for (int i = 0; i < scan_count; i++) {
        pr_info("  [%d] %s  ch%d  signal=%d%% (%d dBm)\n",
                i, scan_results[i].ssid, scan_results[i].channel,
                scan_results[i].signal_pct, scan_results[i].signal_dbm);
    }

    return scan_count;
}

/* ===================================================================
 * Public API — frame receive (called from net.c)
 * =================================================================== */

void wifi_handle_frame(const uint8_t* data, uint16_t len)
{
    if (!initialized || len < 24) return;

    /* Decode Frame Control */
    uint16_t fc     = data[0] | ((uint16_t)data[1] << 8);
    uint8_t  type    = (fc >> 2) & 0x03;   /* 0 = Management */
    uint8_t  subtype = (fc >> 4) & 0x0F;

    if (type != 0) return;  /* only management frames */

    switch (subtype) {

    case MGMT_PROBE_RESP:
        if (scan_count < WIFI_MAX_SCAN) {
            wifi_network_t result;
            memset(&result, 0, sizeof(result));
            if (wifi_parse_probe_response(data, len, &result) == 0) {
                /* Deduplicate by BSSID */
                for (int i = 0; i < scan_count; i++) {
                    if (memcmp(scan_results[i].bssid,
                               result.bssid, 6) == 0)
                        return;
                }
                scan_results[scan_count++] = result;
            }
        }
        break;

    case MGMT_AUTH:
        if (connected && !is_authenticated) {
            if (wifi_parse_auth_response(data, len) == 0) {
                is_authenticated = 1;
                pr_info("wifi: authenticated — sending assoc request\n");
                mac_addr_t our_mac = e1000_get_mac();
                mac_addr_t bssid;
                memcpy(bssid.bytes, assoc_bssid, 6);
                wifi_send_assoc_request(our_mac, bssid,
                                         connected_ssid,
                                         (int)strlen(connected_ssid));
            }
        }
        break;

    case MGMT_ASSOC_RESP:
        if (connected && is_authenticated && !is_associated) {
            uint16_t aid = 0;
            if (wifi_parse_assoc_response(data, len, &aid) == 0) {
                assoc_id = aid;
                is_associated = 1;
                pr_info("wifi: associated (AID=%d)\n", assoc_id);
            }
        }
        break;

    default:
        break;
    }
}

/* ===================================================================
 * Public API — queries
 * =================================================================== */

const char* wifi_get_ssid(int idx)
{
    if (!initialized || idx < 0 || idx >= scan_count) return NULL;
    return scan_results[idx].ssid;
}

int wifi_get_signal(int idx)
{
    if (!initialized || idx < 0 || idx >= scan_count) return -1;
    return scan_results[idx].signal_pct;
}

/* ===================================================================
 * Public API — connect / disconnect
 * =================================================================== */

int wifi_connect(const char* ssid, const char* password)
{
    if (!initialized || !ssid) return -1;

    pr_info("wifi: connect request to \"%s\"\n", ssid);

    if (!e1000_is_present()) {
        pr_warn("wifi: no NIC — cannot connect\n");
        return -1;
    }

    /* Find the network in the most recent scan results */
    int found = -1;
    for (int i = 0; i < scan_count; i++) {
        if (strcmp(scan_results[i].ssid, ssid) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        pr_warn("wifi: \"%s\" not in scan results (run 'wifi scan')\n",
                ssid);
        return -1;
    }

    /* Set up connection state before beginning the 802.11 handshake
     * so that wifi_handle_frame() can drive the state machine. */
    connected     = 1;
    connected_idx = found;
    strncpy(connected_ssid, ssid, WIFI_SSID_LEN - 1);
    connected_ssid[WIFI_SSID_LEN - 1] = '\0';
    memcpy(assoc_bssid, scan_results[found].bssid, 6);
    is_authenticated = 0;
    is_associated    = 0;

    /* --- Step 1: Open-System Authentication --- */
    pr_info("wifi: authenticating (open system) with %s ...\n", ssid);
    mac_addr_t our_mac = e1000_get_mac();
    mac_addr_t bssid;
    memcpy(bssid.bytes, assoc_bssid, 6);
    wifi_send_auth_request(our_mac, bssid);

    /* Wait up to 1 s for the auth response */
    wifi_wait_ms(1000);

    if (!is_authenticated) {
        pr_warn("wifi: authentication timed out\n");
        goto fail;
    }

    /* --- Step 2: Association --- */
    /* (The assoc request was already sent by wifi_handle_frame()
     * upon receiving the auth response.) */
    pr_info("wifi: waiting for association response ...\n");
    wifi_wait_ms(1000);

    if (!is_associated) {
        pr_warn("wifi: association timed out\n");
        goto fail;
    }

    /* --- Step 3: WPA2 4-way handshake (not implemented) --- */
    pr_info("wifi: connected to \"%s\" (AID=%d, signal=%d%%)\n",
            connected_ssid, assoc_id,
            scan_results[found].signal_pct);

    (void)password;  /* WPA2 handshake would go here */
    return 0;

fail:
    connected     = 0;
    connected_idx = -1;
    assoc_id      = 0;
    is_authenticated = 0;
    is_associated    = 0;
    memset(connected_ssid, 0, sizeof(connected_ssid));
    memset(assoc_bssid, 0, sizeof(assoc_bssid));
    return -1;
}

int wifi_disconnect(void)
{
    if (!initialized) return -1;
    if (!connected) {
        pr_info("wifi: not connected — nothing to disconnect\n");
        return 0;
    }

    pr_info("wifi: disconnecting from \"%s\"\n", connected_ssid);
    connected     = 0;
    connected_idx = -1;
    assoc_id      = 0;
    is_authenticated = 0;
    is_associated    = 0;
    memset(connected_ssid, 0, sizeof(connected_ssid));
    memset(assoc_bssid, 0, sizeof(assoc_bssid));
    return 0;
}

int wifi_is_connected(void)
{
    return connected ? 1 : 0;
}

const char* wifi_get_connected_ssid(void)
{
    if (!connected) return NULL;
    return connected_ssid;
}
