/*
 * Lestra OS - Networking Stack
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * A small but real TCP/IP stack for the Lestra kernel, sufficient for
 * DHCP auto-configuration, ICMP ping, DNS lookups, and one-shot HTTP
 * GET/POST requests. Enough to download packages and talk to a local
 * LLM server over plain HTTP.
 *
 * Layout:
 *   net.h        - this file (public surface)
 *   drivers/net/e1000.c  - Intel 82540EM NIC driver (QEMU default)
 *   net/arp.c    - ARP table
 *   net/ip.c     - IPv4 layer + ICMP
 *   net/udp.c    - UDP + DHCP + DNS
 *   net/tcp.c    - minimal TCP state machine for one-shot HTTP
 *   net/http.c   - HTTP/1.0 client (GET + POST)
 *
 * Limitations (intentional, documented):
 *   - Single outstanding TCP connection at a time (sufficient for the
 *     shell's request/response pattern).
 *   - TLS 1.2 client with ECDHE-RSA-AES128-GCM-SHA256, X.509 cert
 *     verification, and RDRAND-backed CSPRNG. HTTPS works end-to-end.
 *   - No fragmentation/reassembly of IP datagrams (we cap MTU at 1500
 *     and assume path MTU is at least that).
 *   - DNS supports A records only (no AAAA, no CNAME chase).
 *   - IPv6 added as dual-stack alongside IPv4 (NDP, ICMPv6, SLAAC).
 */

#ifndef LESTRA_NET_H
#define LESTRA_NET_H

#include <lestra/types.h>

/* ----- addresses ----- */
typedef struct {
    uint8_t bytes[4];
} ipv4_addr_t;

typedef struct {
    uint8_t bytes[6];
} mac_addr_t;

#define MAC_BROADCAST  ((mac_addr_t){{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}})
#define MAC_ZERO       ((mac_addr_t){{0,0,0,0,0,0}})
#define IP_ZERO        ((ipv4_addr_t){{0,0,0,0}})
#define IP_BROADCAST   ((ipv4_addr_t){{0xFF,0xFF,0xFF,0xFF}})

/* handy constructors */
static inline ipv4_addr_t ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    ipv4_addr_t ip; ip.bytes[0]=a; ip.bytes[1]=b; ip.bytes[2]=c; ip.bytes[3]=d; return ip;
}
static inline int ipv4_eq(ipv4_addr_t a, ipv4_addr_t b) {
    return a.bytes[0]==b.bytes[0] && a.bytes[1]==b.bytes[1] &&
           a.bytes[2]==b.bytes[2] && a.bytes[3]==b.bytes[3];
}
static inline int ipv4_is_zero(ipv4_addr_t a) {
    return a.bytes[0]==0 && a.bytes[1]==0 && a.bytes[2]==0 && a.bytes[3]==0;
}

/* ----- IPv6 addresses ----- */
typedef struct {
    uint8_t bytes[16];
} ipv6_addr_t;

#define IPV6_ZERO       ((ipv6_addr_t){{0}})
#define IPV6_LOOPBACK   ((ipv6_addr_t){{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}})
#define IPV6_UNSPEC     ((ipv6_addr_t){{0}})
#define IPV6_ALL_NODES  ((ipv6_addr_t){{0xff,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,1}})
#define IPV6_ALL_ROUTERS ((ipv6_addr_t){{0xff,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,2}})
#define IPV6_LINK_LOCAL_PREFIX ((ipv6_addr_t){{0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0}})

static inline int ipv6_eq(ipv6_addr_t a, ipv6_addr_t b) {
    for (int i = 0; i < 16; i++) if (a.bytes[i] != b.bytes[i]) return 0;
    return 1;
}
static inline int ipv6_is_zero(ipv6_addr_t a) {
    for (int i = 0; i < 16; i++) if (a.bytes[i]) return 0;
    return 1;
}
static inline int ipv6_is_multicast(ipv6_addr_t a) {
    return a.bytes[0] == 0xff;
}
static inline int ipv6_is_link_local(ipv6_addr_t a) {
    return a.bytes[0] == 0xfe && (a.bytes[1] & 0xc0) == 0x80;
}
static inline ipv6_addr_t ipv6_create_link_local(mac_addr_t mac) {
    ipv6_addr_t a = IPV6_LINK_LOCAL_PREFIX;
    /* EUI-64 from MAC: insert 0xff 0xfe in middle, flip 7th bit */
    a.bytes[8]  = mac.bytes[0] ^ 0x02;
    a.bytes[9]  = mac.bytes[1];
    a.bytes[10] = mac.bytes[2];
    a.bytes[11] = 0xff;
    a.bytes[12] = 0xfe;
    a.bytes[13] = mac.bytes[3];
    a.bytes[14] = mac.bytes[4];
    a.bytes[15] = mac.bytes[5];
    return a;
}

/* byte-order helpers (x86_64 is little-endian, network is big-endian) */
static inline uint16_t htons16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint16_t ntohs16(uint16_t v) {
    return htons16(v);
}
static inline uint32_t htonl32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v & 0xFF0000) >> 8) | ((v & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl32(uint32_t v) {
    return htonl32(v);
}

/* ----- packet buffer -----
 * Used everywhere in the stack. The header fields let each layer know
 * where its header ends and the next layer's payload begins. */
#define NET_MAX_PKT  1518   /* Ethernet MTU + 14-byte header */

struct pktbuf {
    uint16_t len;                  /* total bytes in data[] */
    uint16_t eth_hdr_len;          /* 14 normally */
    uint16_t ip_hdr_off;           /* 14 */
    uint16_t ip_hdr_len;           /* 20 normally */
    uint16_t l4_off;               /* ip_hdr_off + ip_hdr_len */
    uint16_t l4_len;               /* len - l4_off */
    uint8_t  data[NET_MAX_PKT];
};

/* ----- public API ----- */

/* Initialize the whole stack: PCI scan for E1000, bring it up, run DHCP. */
void net_init(void);

/* Pump: call from timer IRQ (1 kHz) to advance DHCP/TCP state machines
 * and poll the NIC for incoming packets. */
void net_tick(void);

/* Status queries */
int          net_is_up(void);             /* 1 once DHCP completes */
ipv4_addr_t  net_get_ip(void);
ipv4_addr_t  net_get_gateway(void);
ipv4_addr_t  net_get_mask(void);
ipv4_addr_t  net_get_dns(void);
mac_addr_t   net_get_mac(void);
const char*  net_get_iface_name(void);

/* Static config setters (used by net_config module) */
void net_set_mask(ipv4_addr_t mask);
void net_set_gw(ipv4_addr_t gw);
void net_set_dns(ipv4_addr_t dns);

/* MAC address control (for MAC randomization) */
int net_set_mac(mac_addr_t mac);

/* ICMP */
int net_ping(ipv4_addr_t target, uint16_t seq, uint32_t timeout_ms);

/* IPv6 ICMPv6 ping */
int net_ping6(ipv6_addr_t target, uint16_t seq, uint32_t timeout_ms);

/* IPv6 status */
int          net_ipv6_is_valid(void);
ipv6_addr_t  net_get_ipv6(void);
ipv6_addr_t  net_get_ipv6_gw(void);

/* DNS — returns 1 on success and fills *out, 0 on failure. */
int net_resolve(const char* hostname, ipv4_addr_t* out);
/* DNS dual-stack: tries AAAA then A. Fills whichever is available. Returns 1 if at least one resolved. */
int net_resolve_dual(const char* hostname, ipv4_addr_t* out4, ipv6_addr_t* out6);

/* HTTP client — does a single request/response and returns the body.
 * Caller provides response buffer; returns total bytes written, or
 * negative on error. */
#define HTTP_MAX_URL    256
struct http_response {
    int      status;          /* HTTP status code (e.g. 200) */
    size_t   body_len;        /* bytes written to body[] */
    char     body[8192];      /* response body (truncated if too big) */
};

int http_get(const char* url, struct http_response* resp);
int http_post(const char* url,
              const char* content_type,
              const char* body,
              size_t body_len,
              struct http_response* resp);

/* Helper: parse "host:port/path" out of a URL. Returns 0 on success. */
int http_parse_url(const char* url,
                   char* scheme_out, int scheme_sz,
                   char* host_out,  int host_sz,
                   uint16_t* port_out,
                   char* path_out,  int path_sz);

/* ----- IPv6 ----- */
struct ipv6_hdr {
    uint32_t   ver_traffic_flow;  /* version(4) + traffic class(8) + flow label(20) */
    uint16_t   payload_len;
    uint8_t    next_header;       /* protocol (TCP=6, UDP=17, ICMPv6=58) */
    uint8_t    hop_limit;
    ipv6_addr_t src;
    ipv6_addr_t dst;
} __packed;

#define IPV6_PROTO_ICMPV6 58
#define IPV6_PROTO_TCP    6
#define IPV6_PROTO_UDP    17

/* ICMPv6 types */
#define ICMPV6_TYPE_ECHO_REQUEST    128
#define ICMPV6_TYPE_ECHO_REPLY      129
#define ICMPV6_TYPE_RS              133
#define ICMPV6_TYPE_RA              134
#define ICMPV6_TYPE_NS              135
#define ICMPV6_TYPE_NA              136

struct icmpv6_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
} __packed;

struct icmpv6_echo {
    uint16_t id;
    uint16_t seq;
} __packed;

struct icmpv6_ns {
    uint32_t reserved;
    ipv6_addr_t target;
    /* options follow */
} __packed;

struct icmpv6_na {
    uint32_t flags;  /* R, S, O flags in high bits */
    ipv6_addr_t target;
    /* options follow */
} __packed;

struct icmpv6_ra {
    uint8_t  cur_hop_limit;
    uint8_t  flags;         /* M, O flags */
    uint16_t router_lifetime;
    uint32_t reachable_time;
    uint32_t retrans_timer;
    /* options follow */
} __packed;

/* NDP option types */
#define NDP_OPT_SOURCE_LLADDR  1
#define NDP_OPT_TARGET_LLADDR  2
#define NDP_OPT_PREFIX_INFO    3

struct ndp_option {
    uint8_t  type;
    uint8_t  length;    /* length in units of 8 bytes */
    /* data follows */
} __packed;

struct ndp_prefix_info {
    uint8_t  type;          /* 3 */
    uint8_t  length;        /* 4 */
    uint8_t  prefix_len;
    uint8_t  flags;         /* L, A bits */
    uint32_t valid_lifetime;
    uint32_t preferred_lifetime;
    uint32_t reserved;
    ipv6_addr_t prefix;
} __packed;

/* ----- NDP cache ----- */
#define NDP_CACHE_SIZE  8
struct ndp_cache_entry {
    ipv6_addr_t ip;
    mac_addr_t  mac;
    int         state;      /* 0=unused, 1=stale, 2=reachable */
    uint64_t    last_seen;
};
static inline void ipv6_addr_to_str(ipv6_addr_t a, char* out, int outsz) {
    /* Print compressed hex form: e.g. "fe80::1" or "2001:db8::1" */
    /* Find longest run of zero groups for :: compression */
    int run_start = -1, run_len = 0, best_start = -1, best_len = 0;
    int i;
    for (i = 0; i < 8; i++) {
        uint16_t g = ((uint16_t)a.bytes[i*2] << 8) | a.bytes[i*2+1];
        if (g == 0) {
            if (run_start < 0) { run_start = i; run_len = 1; }
            else run_len++;
        } else {
            if (run_len > best_len) { best_start = run_start; best_len = run_len; }
            run_start = -1; run_len = 0;
        }
    }
    if (run_len > best_len) { best_start = run_start; best_len = run_len; }

    char* w = out;
    char* end = out + outsz - 1;
    for (i = 0; i < 8; i++) {
        if (best_start >= 0 && i == best_start) {
            if (i == 0 && best_start == 0) {
                if (w < end) *w++ = ':';
            }
            if (w < end) *w++ = ':';
            i += best_len - 1;
            continue;
        }
        uint16_t hword = ((uint16_t)a.bytes[i*2] << 8) | a.bytes[i*2+1];
        /* Convert nibbles to hex */
        static const char hex[] = "0123456789abcdef";
        int started = 0;
        for (int s = 12; s >= 0; s -= 4) {
            int nib = (hword >> s) & 0xF;
            if (nib || started || s == 0) {
                if (w < end) *w++ = hex[nib];
                started = 1;
            }
        }
        if (i < 7) {
            if (w < end) *w++ = ':';
        }
    }
    *w = '\0';
}

/* ----- public API ----- */
typedef enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_CLOSING,
    TCP_LISTEN,
} tcp_state_t;

#define TCP_MAX_CONNS 8

struct tcp_conn {
    int in_use;
    int fd;
    tcp_state_t state;
    ipv4_addr_t peer_ip;
    ipv6_addr_t peer_ip6;
    int         is_ipv6;
    uint16_t peer_port;
    uint16_t local_port;
    uint32_t tx_seq;
    uint32_t rx_seq;
    uint8_t rx_buf[8192];
    uint16_t rx_len;
    int rx_closed;
    int tcp_connected;
    int tcp_connect_failed;
    int retransmit_count;
    uint8_t last_seg[1540];
    uint16_t last_seg_len;
    uint64_t last_seg_time;
    int is_server;
    int pending_accept;
    struct tcp_conn* accepted;
};

/* ----- TCP API ----- */
/* Client (uses conn[0] internally) */
int  tcp_connect(ipv4_addr_t dst_ip, uint16_t dst_port, uint32_t timeout_ms);
int  tcp_connect6(ipv6_addr_t dst_ip, uint16_t dst_port, uint32_t timeout_ms);
int  tcp_send(const void* data, uint16_t len);
int  tcp_recv_wait(uint8_t* buf, uint16_t bufsz, uint32_t timeout_ms);
void tcp_close(void);
int  tcp_is_closed(void);

/* Server */
int  tcp_listen(uint16_t port, int backlog);
int  tcp_accept(int listen_idx, struct tcp_conn** out_conn);

/* Connection-specific */
int  tcp_send_conn(struct tcp_conn* c, const void* data, uint16_t len);
int  tcp_recv_conn(struct tcp_conn* c, uint8_t* buf, uint16_t bufsz, uint32_t timeout_ms);
void tcp_close_conn(struct tcp_conn* c);
struct tcp_conn* tcp_get_conn(int idx);

/* ----- UDP API (used by DNS / DHCP) ----- */
int  udp_send(ipv4_addr_t dst_ip, uint16_t src_port, uint16_t dst_port,
              const void* payload, uint16_t payload_len);
int  udp_send6(ipv6_addr_t dst_ip, uint16_t src_port, uint16_t dst_port,
               const void* payload, uint16_t payload_len);
int  udp_recv_wait(uint8_t* buf, uint16_t bufsz, uint32_t timeout_ms);

#endif /* LESTRA_NET_H */
