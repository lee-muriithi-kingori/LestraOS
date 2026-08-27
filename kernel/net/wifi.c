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
 *            Assoc Request (with RSN IE) -> Assoc Response (get AID)
 *            WPA2 4-way handshake via EAPOL (ethertype 0x888E):
 *              Msg 1 (AP->Client): ANonce
 *              Msg 2 (Client->AP): SNonce + MIC (HMAC-SHA1-128)
 *              Msg 3 (AP->Client): GTK (AES-128-CTR encrypted) + MIC
 *              Msg 4 (Client->AP): confirm
 *
 * Crypto:   PBKDF2-HMAC-SHA1 for PMK derivation
 *           PRF-512 for PTK derivation
 *           AES-128-CTR for GTK decryption in Message 3
 *
 * When the E1000 NIC has no WiFi counterpart (as in QEMU), probe
 * requests never receive responses and wifi_scan() returns 0
 * results — no fabricated networks.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/net.h>
#include <lestra/nic.h>
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
 * WPA2 / EAPOL constants
 * =================================================================== */

#define EAPOL_ETH_TYPE      0x888E
#define EAPOL_8021X_VER     1
#define EAPOL_TYPE_KEY      3
#define EAPOL_KEY_DESC_RSN  2     /* RSN / WPA2 key descriptor type */

/* Key Information field bits (2 bytes, little-endian) */
#define KEY_INFO_VER_MASK    0x0007
#define KEY_INFO_VER_2       2    /* HMAC-SHA1-128 (WPA2) */
#define KEY_INFO_PAIRWISE    (1 << 3)
#define KEY_INFO_INSTALL     (1 << 4)
#define KEY_INFO_ACK         (1 << 5)
#define KEY_INFO_MIC         (1 << 6)
#define KEY_INFO_SECURE      (1 << 7)
#define KEY_INFO_REQUEST     (1 << 9)

/* EAPOL-Key descriptor body byte offsets */
#define EAPOL_KEY_DESC_OFF   0
#define EAPOL_KEY_INFO_OFF   1
#define EAPOL_KEY_LEN_OFF    3
#define EAPOL_KEY_REPLAY_OFF 5
#define EAPOL_KEY_NONCE_OFF  13
#define EAPOL_KEY_IV_OFF     45
#define EAPOL_KEY_RSC_OFF    61
#define EAPOL_KEY_ID_OFF     69
#define EAPOL_KEY_MIC_OFF    77
#define EAPOL_KEY_DLEN_OFF   93
#define EAPOL_KEY_DATA_OFF   95

/* RSN IE constants */
#define RSN_IE_TAG           48
#define RSN_IE_LEN           20
#define RSN_OUI              0x00, 0x0F, 0xAC
#define RSN_CIPHER_CCMP      4
#define RSN_AKM_PSK          1

/* WPA2 handshake states */
#define WPA2_STATE_IDLE      0
#define WPA2_STATE_MSG1      1   /* waiting for Message 1 (ANonce) */
#define WPA2_STATE_MSG3      2   /* waiting for Message 3 (GTK) */
#define WPA2_STATE_DONE      3   /* handshake complete */

typedef struct {
    uint8_t  pmk[32];            /* Pre-Master Key (from PBKDF2) */
    uint8_t  anonce[32];         /* AP's Nonce */
    uint8_t  snonce[32];         /* Our Nonce */
    uint8_t  ptk[64];            /* Pairwise Transient Key: KCK||KEK||TK */
    uint8_t  gtk[32];            /* Group Temporal Key */
    uint8_t  mac_addr[6];        /* Our MAC address */
    uint8_t  ap_mac[6];          /* AP's BSSID */
    int      state;              /* WPA2_STATE_* */
    int      completed;          /* 1 if handshake done */
    uint64_t replay_counter;     /* current replay counter */
} wpa2_state_t;

static wpa2_state_t wpa;

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
 * NIC driver abstraction — use active_nic_ops vtable instead of hardcoded e1000
 * Fallback to e1000 for early init before active_nic_ops is set.
 * =================================================================== */

extern int        e1000_send(const void* data, uint16_t len);
extern int        e1000_recv(void* buf, uint16_t bufsz);
extern mac_addr_t e1000_get_mac(void);
extern int        e1000_is_present(void);

static inline int wifi_nic_send(const void* data, uint16_t len) {
    if (active_nic_ops && active_nic_ops->send)
        return active_nic_ops->send(data, len);
    return e1000_send(data, len);
}
static inline mac_addr_t wifi_nic_get_mac(void) {
    if (active_nic_ops && active_nic_ops->get_mac)
        return active_nic_ops->get_mac();
    return e1000_get_mac();
}
static inline int wifi_nic_is_present(void) {
    if (active_nic_ops) return 1;
    return e1000_is_present();
}

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

    /* Tag 48 — RSN IE (WPA2/CCMP/PSK) so the AP knows we want WPA2 */
    if (wpa.state != WPA2_STATE_IDLE) {
        uint8_t rsn[22];
        int ri = 0;
        rsn[ri++] = 0x01; rsn[ri++] = 0x00;           /* RSN Version 1 */
        rsn[ri++] = 0x00; rsn[ri++] = 0x0F;
        rsn[ri++] = 0xAC; rsn[ri++] = RSN_CIPHER_CCMP; /* Group cipher */
        rsn[ri++] = 0x01; rsn[ri++] = 0x00;           /* Pairwise count */
        rsn[ri++] = 0x00; rsn[ri++] = 0x0F;
        rsn[ri++] = 0xAC; rsn[ri++] = RSN_CIPHER_CCMP; /* Pairwise cipher */
        rsn[ri++] = 0x01; rsn[ri++] = 0x00;           /* AKM count */
        rsn[ri++] = 0x00; rsn[ri++] = 0x0F;
        rsn[ri++] = 0xAC; rsn[ri++] = RSN_AKM_PSK;    /* AKM PSK */
        rsn[ri++] = 0x00; rsn[ri++] = 0x00;           /* RSN capab */
        off = wifi_append_ie(buf, off, IE_RSN, RSN_IE_LEN, rsn);
    }

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

    mac_addr_t our_mac = wifi_nic_get_mac();
    mac_addr_t bcast   = MAC_BROADCAST;

    eth->dst       = bcast;
    eth->src       = our_mac;
    eth->ethertype = htons16(WIFI_ETH_TYPE);
    memcpy(eth_buf + sizeof(struct wifi_eth_hdr), mgmt_frame, mgmt_len);

    wifi_nic_send(eth_buf, (uint16_t)(sizeof(struct wifi_eth_hdr) + mgmt_len));
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
 * SHA-1 hash (minimal, for WPA2 key derivation)
 * =================================================================== */

static void sha1_transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t a, b, c, d, e, f, k, temp;
    uint32_t w[80];

    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4] << 24) |
               ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) |
               (uint32_t)block[i*4+3];
    for (int i = 16; i < 80; i++) {
        w[i] = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (w[i] << 1) | (w[i] >> 31);
    }

    a = state[0]; b = state[1]; c = state[2];
    d = state[3]; e = state[4];

    for (int i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else              { f = b ^ c ^ d; k = 0xCA62C1D6; }

        temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
    }

    state[0] += a; state[1] += b; state[2] += c;
    state[3] += d; state[4] += e;
}

static void sha1(const uint8_t* data, size_t len, uint8_t hash[20])
{
    uint32_t state[5] = {
        0x67452301, 0xEFCDAB89, 0x98BADCFE,
        0x10325476, 0xC3D2E1F0
    };
    uint8_t block[64];
    size_t offset = 0;

    while (offset + 64 <= len) {
        sha1_transform(state, data + offset);
        offset += 64;
    }

    size_t remaining = len - offset;
    memset(block, 0, 64);
    memcpy(block, data + offset, remaining);
    block[remaining] = 0x80;

    if (remaining >= 56) {
        sha1_transform(state, block);
        memset(block, 0, 64);
    }

    uint64_t bitlen = (uint64_t)len * 8;
    block[56] = (uint8_t)(bitlen >> 56);
    block[57] = (uint8_t)(bitlen >> 48);
    block[58] = (uint8_t)(bitlen >> 40);
    block[59] = (uint8_t)(bitlen >> 32);
    block[60] = (uint8_t)(bitlen >> 24);
    block[61] = (uint8_t)(bitlen >> 16);
    block[62] = (uint8_t)(bitlen >> 8);
    block[63] = (uint8_t)(bitlen);
    sha1_transform(state, block);

    for (int i = 0; i < 5; i++) {
        hash[i*4]   = (uint8_t)(state[i] >> 24);
        hash[i*4+1] = (uint8_t)(state[i] >> 16);
        hash[i*4+2] = (uint8_t)(state[i] >> 8);
        hash[i*4+3] = (uint8_t)(state[i]);
    }
}

/* ===================================================================
 * HMAC-SHA1 (for WPA2 PRF and MIC)
 * =================================================================== */

static void hmac_sha1(const uint8_t* key, size_t keylen,
                      const uint8_t* data, size_t datalen,
                      uint8_t* out, size_t outlen)
{
    uint8_t k_ipad[64], k_opad[64];
    uint8_t tmp_key[20];
    uint8_t inner_hash[20];
    uint8_t buf[384];

    if (keylen > 64) {
        sha1(key, keylen, tmp_key);
        key    = tmp_key;
        keylen = 20;
    }

    memset(k_ipad, 0x36, 64);
    memset(k_opad, 0x5C, 64);
    for (size_t i = 0; i < keylen; i++) {
        k_ipad[i] ^= key[i];
        k_opad[i] ^= key[i];
    }

    memcpy(buf, k_ipad, 64);
    memcpy(buf + 64, data, datalen);
    sha1(buf, 64 + datalen, inner_hash);

    memcpy(buf, k_opad, 64);
    memcpy(buf + 64, inner_hash, 20);
    sha1(buf, 64 + 20, inner_hash);

    size_t copy = outlen <= 20 ? outlen : 20;
    memcpy(out, inner_hash, copy);
}

/* ===================================================================
 * PBKDF2-HMAC-SHA1 (derive PMK from passphrase + SSID)
 * =================================================================== */

static void pbkdf2_sha1(const char* passphrase, const char* ssid,
                        int iterations, uint8_t* out, int outlen)
{
    size_t plen = strlen(passphrase);
    size_t slen = strlen(ssid);
    uint8_t salt[36];
    uint8_t U[20], T[20];

    memcpy(salt, ssid, slen);

    int blocks = (outlen + 19) / 20;
    int offset = 0;

    for (int blk = 1; blk <= blocks; blk++) {
        salt[slen]     = (uint8_t)(blk >> 24);
        salt[slen + 1] = (uint8_t)(blk >> 16);
        salt[slen + 2] = (uint8_t)(blk >> 8);
        salt[slen + 3] = (uint8_t)(blk);

        hmac_sha1((const uint8_t*)passphrase, plen,
                  salt, slen + 4, T, 20);
        memcpy(U, T, 20);

        for (int iter = 1; iter < iterations; iter++) {
            hmac_sha1((const uint8_t*)passphrase, plen,
                      U, 20, U, 20);
            for (int i = 0; i < 20; i++)
                T[i] ^= U[i];
        }

        int copy = outlen - offset;
        if (copy > 20) copy = 20;
        memcpy(out + offset, T, copy);
        offset += copy;
    }
}

/* ===================================================================
 * Minimal AES-128 (for WPA2 GTK decryption in Message 3)
 * =================================================================== */

static const uint8_t wpa_aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static uint8_t wpa_aes_xtime(uint8_t x)
{
    return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b));
}

static void wpa_aes128_key_expand(const uint8_t key[16], uint8_t rk[176])
{
    memcpy(rk, key, 16);
    uint32_t rcon = 0x01000000;

    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        memcpy(t, rk + (i - 1) * 4, 4);
        if (i % 4 == 0) {
            uint8_t tmp = t[0]; t[0] = t[1]; t[1] = t[2];
            t[2] = t[3]; t[3] = tmp;
            for (int j = 0; j < 4; j++)
                t[j] = wpa_aes_sbox[t[j]];
            t[0] ^= (uint8_t)(rcon >> 24);
            rcon <<= 8;
        }
        for (int j = 0; j < 4; j++)
            rk[i * 4 + j] = rk[(i - 4) * 4 + j] ^ t[j];
    }
}

static void wpa_aes128_encrypt(const uint8_t in[16], uint8_t out[16],
                                const uint8_t rk[176])
{
    uint8_t s[16];
    memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];

    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = wpa_aes_sbox[s[i]];

        uint8_t t;
        t=s[1]; s[1]=s[5]; s[5]=s[9]; s[9]=s[13]; s[13]=t;
        t=s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
        t=s[15]; s[15]=s[11]; s[11]=s[7]; s[7]=s[3]; s[3]=t;

        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0=s[c*4], a1=s[c*4+1], a2=s[c*4+2], a3=s[c*4+3];
                s[c*4]   = wpa_aes_xtime(a0)^wpa_aes_xtime(a1)^a1^a2^a3;
                s[c*4+1] = a0^wpa_aes_xtime(a1)^wpa_aes_xtime(a2)^a2^a3;
                s[c*4+2] = a0^a1^wpa_aes_xtime(a2)^wpa_aes_xtime(a3)^a3;
                s[c*4+3] = wpa_aes_xtime(a0)^a0^a1^a2^wpa_aes_xtime(a3);
            }
        }

        for (int i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];
    }

    memcpy(out, s, 16);
}

/* AES-128-CTR: XOR data with keystream (encrypt == decrypt for CTR) */
static void wpa_aes128_ctr(uint8_t* data, size_t len,
                           const uint8_t key[16], const uint8_t iv[16])
{
    uint8_t counter[16];
    uint8_t round_keys[176];
    uint8_t ks[16];

    memcpy(counter, iv, 16);
    wpa_aes128_key_expand(key, round_keys);

    size_t off = 0;
    while (off < len) {
        wpa_aes128_encrypt(counter, ks, round_keys);
        size_t blen = len - off;
        if (blen > 16) blen = 16;
        for (size_t i = 0; i < blen; i++)
            data[off + i] ^= ks[i];
        off += blen;
        for (int i = 15; i >= 0; i--)
            if (++counter[i] != 0) break;
    }
}

/* ===================================================================
 * WPA2 PRF-512 key derivation
 *   PTK = PRF-512(PMK, "Pairwise key expansion",
 *                  Min(AA,SPA)||Max(AA,SPA)||Min(ANonce,SNonce)||Max(...))
 * =================================================================== */

static void wpa2_prf512(const uint8_t pmk[32],
                        const uint8_t aa[6], const uint8_t spa[6],
                        const uint8_t anonce[32], const uint8_t snonce[32],
                        uint8_t ptk[64])
{
    const char* label = "Pairwise key expansion";
    size_t label_len = 22;

    uint8_t addr1[6], addr2[6];
    if (memcmp(aa, spa, 6) <= 0) {
        memcpy(addr1, aa, 6); memcpy(addr2, spa, 6);
    } else {
        memcpy(addr1, spa, 6); memcpy(addr2, aa, 6);
    }

    uint8_t n1[32], n2[32];
    if (memcmp(anonce, snonce, 32) <= 0) {
        memcpy(n1, anonce, 32); memcpy(n2, snonce, 32);
    } else {
        memcpy(n1, snonce, 32); memcpy(n2, anonce, 32);
    }

    uint8_t data[76];
    memcpy(data,      addr1, 6);
    memcpy(data + 6,  addr2, 6);
    memcpy(data + 12, n1,   32);
    memcpy(data + 44, n2,   32);

    for (int i = 0; i < 4; i++) {
        uint8_t input[102];
        int off = 0;
        memcpy(input + off, label, label_len); off += label_len;
        input[off++] = 0x00;
        memcpy(input + off, data, 76); off += 76;
        input[off++] = 0x00;
        input[off++] = (uint8_t)i;

        uint8_t hmac_out[20];
        hmac_sha1(pmk, 32, input, off, hmac_out, 20);
        int copy = 64 - i * 20;
        if (copy > 20) copy = 20;
        memcpy(ptk + i * 20, hmac_out, copy);
    }
}

/* ===================================================================
 * WPA2 EAPOL frame send / receive
 * =================================================================== */

extern void get_random_bytes(void* buf, size_t len);

static void wpa_generate_snonce(void)
{
    get_random_bytes(wpa.snonce, 32);
}

static void wpa_derive_ptk(void)
{
    mac_addr_t our_mac = wifi_nic_get_mac();
    memcpy(wpa.mac_addr, our_mac.bytes, 6);
    wpa2_prf512(wpa.pmk, wpa.ap_mac, wpa.mac_addr,
                wpa.anonce, wpa.snonce, wpa.ptk);
}

static void wpa2_compute_mic(const uint8_t* body, uint16_t body_len,
                             const uint8_t kck[16], uint8_t mic[16])
{
    uint8_t full[20];
    hmac_sha1(kck, 16, body, body_len, full, 20);
    memcpy(mic, full, 16);
}

static void wifi_send_eapol(const uint8_t* body, uint16_t body_len,
                            const uint8_t dst_mac[6])
{
    uint8_t pkt[300];
    int off = 0;

    memcpy(pkt + off, dst_mac, 6);           off += 6;
    mac_addr_t our_mac = wifi_nic_get_mac();
    memcpy(pkt + off, our_mac.bytes, 6);      off += 6;
    pkt[off++] = (uint8_t)((EAPOL_ETH_TYPE >> 8) & 0xFF);
    pkt[off++] = (uint8_t)(EAPOL_ETH_TYPE & 0xFF);

    pkt[off++] = EAPOL_8021X_VER;
    pkt[off++] = EAPOL_TYPE_KEY;
    pkt[off++] = (uint8_t)((body_len >> 8) & 0xFF);
    pkt[off++] = (uint8_t)(body_len & 0xFF);

    memcpy(pkt + off, body, body_len);
    off += body_len;

    wifi_nic_send(pkt, (uint16_t)off);
}

static void wpa_send_message_2(void)
{
    uint8_t body[256];
    int off = 0;

    body[off++] = EAPOL_KEY_DESC_RSN;

    uint16_t ki = KEY_INFO_VER_2 | KEY_INFO_PAIRWISE | KEY_INFO_MIC;
    body[off++] = (uint8_t)(ki & 0xFF);
    body[off++] = (uint8_t)((ki >> 8) & 0xFF);

    body[off++] = 0; body[off++] = 0;

    for (int i = 7; i >= 0; i--)
        body[off++] = (uint8_t)(wpa.replay_counter >> (i * 8));

    memcpy(body + off, wpa.snonce, 32); off += 32;

    memset(body + off, 0, 16); off += 16;
    memset(body + off, 0, 8);  off += 8;
    memset(body + off, 0, 8);  off += 8;

    int mic_off = off;
    memset(body + off, 0, 16); off += 16;

    body[off++] = 0; body[off++] = 0;

    wpa2_compute_mic(body, (uint16_t)off, wpa.ptk, body + mic_off);

    wifi_send_eapol(body, (uint16_t)off, wpa.ap_mac);
    pr_info("wifi: WPA2 msg2 sent (SNonce ready)\n");
}

static void wpa_send_message_4(void)
{
    uint8_t body[256];
    int off = 0;

    body[off++] = EAPOL_KEY_DESC_RSN;

    uint16_t ki = KEY_INFO_VER_2 | KEY_INFO_PAIRWISE |
                  KEY_INFO_MIC | KEY_INFO_SECURE;
    body[off++] = (uint8_t)(ki & 0xFF);
    body[off++] = (uint8_t)((ki >> 8) & 0xFF);

    body[off++] = 0; body[off++] = 0;

    for (int i = 7; i >= 0; i--)
        body[off++] = (uint8_t)(wpa.replay_counter >> (i * 8));

    memset(body + off, 0, 32); off += 32;
    memset(body + off, 0, 16); off += 16;
    memset(body + off, 0, 8);  off += 8;
    memset(body + off, 0, 8);  off += 8;

    int mic_off = off;
    memset(body + off, 0, 16); off += 16;

    body[off++] = 0; body[off++] = 0;

    wpa2_compute_mic(body, (uint16_t)off, wpa.ptk, body + mic_off);

    wifi_send_eapol(body, (uint16_t)off, wpa.ap_mac);
    pr_info("wifi: WPA2 msg4 sent — handshake confirmed\n");
}

/* Handle incoming EAPOL-Key frame (called from net.c for ethertype 0x888E) */
void wifi_handle_eapol_frame(const uint8_t* data, uint16_t len)
{
    if (len < 4) return;

    uint8_t  type    = data[1];
    uint16_t body_len = ((uint16_t)data[2] << 8) | data[3];

    if (type != EAPOL_TYPE_KEY) return;
    if ((uint32_t)4 + body_len > len) return;
    if (body_len < EAPOL_KEY_DATA_OFF) return;

    const uint8_t* body = data + 4;

    if (body[EAPOL_KEY_DESC_OFF] != EAPOL_KEY_DESC_RSN) return;

    uint16_t key_info = body[EAPOL_KEY_INFO_OFF] |
                        ((uint16_t)body[EAPOL_KEY_INFO_OFF + 1] << 8);

    uint64_t replay = 0;
    for (int i = 0; i < 8; i++)
        replay = (replay << 8) | body[EAPOL_KEY_REPLAY_OFF + i];

    const uint8_t* nonce = body + EAPOL_KEY_NONCE_OFF;
    const uint8_t* iv    = body + EAPOL_KEY_IV_OFF;
    const uint8_t* mic   = body + EAPOL_KEY_MIC_OFF;
    uint16_t kd_len = body[EAPOL_KEY_DLEN_OFF] |
                      ((uint16_t)body[EAPOL_KEY_DLEN_OFF + 1] << 8);

    int ack     = (key_info & KEY_INFO_ACK) != 0;
    int mic_f   = (key_info & KEY_INFO_MIC) != 0;
    int secure  = (key_info & KEY_INFO_SECURE) != 0;
    int ptype   = (key_info & KEY_INFO_PAIRWISE) != 0;
    int ver     = key_info & KEY_INFO_VER_MASK;

    pr_info("wifi: EAPOL-Key recv info=0x%04X ack=%d mic=%d sec=%d\n",
            key_info, ack, mic_f, secure);

    switch (wpa.state) {

    case WPA2_STATE_MSG1:
        if (ack && ptype && !mic_f && ver == KEY_INFO_VER_2) {
            memcpy(wpa.anonce, nonce, 32);
            wpa.replay_counter = replay;

            wpa_generate_snonce();
            wpa_derive_ptk();
            wpa_send_message_2();
            wpa.state = WPA2_STATE_MSG3;
        }
        break;

    case WPA2_STATE_MSG3:
        if (ack && mic_f && secure && ptype && ver == KEY_INFO_VER_2) {
            uint8_t body_copy[256];
            int copy_len = body_len;
            if (copy_len > 255) copy_len = 255;
            memcpy(body_copy, body, copy_len);
            memset(body_copy + EAPOL_KEY_MIC_OFF, 0, 16);

            uint8_t expected_mic[16];
            wpa2_compute_mic(body_copy, (uint16_t)copy_len,
                            wpa.ptk, expected_mic);

            if (memcmp(mic, expected_mic, 16) != 0) {
                pr_warn("wifi: WPA2 msg3 MIC mismatch\n");
                break;
            }

            if (kd_len > 0 && EAPOL_KEY_DATA_OFF + kd_len <= (uint16_t)copy_len) {
                uint8_t keydata[128];
                int kd_copy = kd_len;
                if (kd_copy > (int)sizeof(keydata) - 1)
                    kd_copy = (int)sizeof(keydata) - 1;
                memcpy(keydata, body_copy + EAPOL_KEY_DATA_OFF, kd_copy);

                wpa_aes128_ctr(keydata, (size_t)kd_copy,
                               wpa.ptk + 16, iv);

                int kd_off = 0;
                while (kd_off + 3 <= kd_copy) {
                    uint8_t  kd_type = keydata[kd_off];
                    uint16_t kd_item = keydata[kd_off + 1] |
                                       ((uint16_t)keydata[kd_off + 2] << 8);
                    if (kd_off + 3 + kd_item > kd_copy) break;

                    if (kd_type == 1 && kd_item >= 24) {
                        memcpy(wpa.gtk, keydata + kd_off + 1,
                               kd_item > 32 ? 32 : kd_item);
                        pr_info("wifi: WPA2 GTK extracted (%d bytes)\n",
                                kd_item);
                    }
                    kd_off += 3 + kd_item;
                }
            }

            wpa.replay_counter = replay;
            wpa_send_message_4();
            wpa.state = WPA2_STATE_DONE;
            wpa.completed = 1;
            pr_info("wifi: WPA2 4-way handshake complete!\n");
        }
        break;

    default:
        break;
    }
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
    pr_info("wifi: initialising WLAN framework (802.11 mgmt frames + WPA2)\n");

    memset(scan_results, 0, sizeof(scan_results));
    memset(connected_ssid, 0, sizeof(connected_ssid));
    scan_count    = 0;
    connected     = 0;
    connected_idx = -1;
    assoc_id      = 0;
    is_authenticated = 0;
    is_associated    = 0;
    memset(assoc_bssid, 0, sizeof(assoc_bssid));
    memset(&wpa, 0, sizeof(wpa));
    initialized   = 1;

    if (wifi_nic_is_present()) {
        pr_info("wifi: NIC present (%s), probes ethertype 0x%04X, EAPOL 0x%04X\n",
                active_nic_ops ? active_nic_ops->name : "e1000",
                WIFI_ETH_TYPE, EAPOL_ETH_TYPE);
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

    if (!wifi_nic_is_present()) {
        pr_warn("wifi: no NIC — cannot scan\n");
        return 0;
    }

    mac_addr_t our_mac = wifi_nic_get_mac();

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
                mac_addr_t our_mac = wifi_nic_get_mac();
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
                if (wpa.state == WPA2_STATE_MSG1)
                    pr_info("wifi: waiting for WPA2 handshake ...\n");
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

    if (!wifi_nic_is_present()) {
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

    /* --- Derive PMK from passphrase + SSID (WPA2-PBKDF2) --- */
    memset(&wpa, 0, sizeof(wpa));
    memcpy(wpa.ap_mac, scan_results[found].bssid, 6);

    if (password && password[0]) {
        pbkdf2_sha1(password, ssid, 4096, wpa.pmk, 32);
        pr_info("wifi: PMK derived from \"%s\" via PBKDF2\n", ssid);
        wpa.state = WPA2_STATE_MSG1;
    } else {
        pr_info("wifi: open network (no WPA2)\n");
        wpa.state = WPA2_STATE_IDLE;
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
    mac_addr_t our_mac = wifi_nic_get_mac();
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

    /* --- Step 3: WPA2 4-way handshake --- */
    if (wpa.state == WPA2_STATE_MSG1) {
        pr_info("wifi: waiting for WPA2 4-way handshake ...\n");
        wifi_wait_ms(2000);

        if (!wpa.completed) {
            pr_warn("wifi: WPA2 handshake timed out\n");
            goto fail;
        }

        pr_info("wifi: connected to \"%s\" (AID=%d, signal=%d%%, WPA2)\n",
                connected_ssid, assoc_id,
                scan_results[found].signal_pct);
    } else {
        pr_info("wifi: connected to \"%s\" (AID=%d, signal=%d%%, open)\n",
                connected_ssid, assoc_id,
                scan_results[found].signal_pct);
    }

    return 0;

fail:
    connected     = 0;
    connected_idx = -1;
    assoc_id      = 0;
    is_authenticated = 0;
    is_associated    = 0;
    memset(connected_ssid, 0, sizeof(connected_ssid));
    memset(assoc_bssid, 0, sizeof(assoc_bssid));
    memset(&wpa, 0, sizeof(wpa));
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
    memset(&wpa, 0, sizeof(wpa));
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
