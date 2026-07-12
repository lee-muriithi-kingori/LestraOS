/*
 * Lestra OS - Networking Stack
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
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
 *   - No HTTPS/TLS. Cloud AI APIs (GLM/Claude) are HTTPS-only — for those,
 *     run a local TLS-terminating proxy (e.g. socat/nginx) and point
 *     `ai setendpoint http://<proxy-host>:<port>/` at it. The HTTP client
 *     works direct against any plain-HTTP endpoint (Ollama, llama.cpp
 *     server, vLLM, or your own relay).
 *   - Single outstanding TCP connection at a time (sufficient for the
 *     shell's request/response pattern).
 *   - No fragmentation/reassembly of IP datagrams (we cap MTU at 1500
 *     and assume path MTU is at least that).
 *   - DNS supports A records only (no AAAA, no CNAME chase).
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
ipv4_addr_t  net_get_dns(void);
mac_addr_t   net_get_mac(void);
const char*  net_get_iface_name(void);

/* ICMP */
int net_ping(ipv4_addr_t target, uint16_t seq, uint32_t timeout_ms);

/* DNS — returns 1 on success and fills *out, 0 on failure. */
int net_resolve(const char* hostname, ipv4_addr_t* out);

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

/* ----- TCP API (used by HTTP client) -----
 * One connection at a time. */
int  tcp_connect(ipv4_addr_t dst_ip, uint16_t dst_port, uint32_t timeout_ms);
int  tcp_send(const void* data, uint16_t len);
int  tcp_recv_wait(uint8_t* buf, uint16_t bufsz, uint32_t timeout_ms);
void tcp_close(void);
int  tcp_is_closed(void);

/* ----- UDP API (used by DNS / DHCP) ----- */
int  udp_send(ipv4_addr_t dst_ip, uint16_t src_port, uint16_t dst_port,
              const void* payload, uint16_t payload_len);
int  udp_recv_wait(uint8_t* buf, uint16_t bufsz, uint32_t timeout_ms);

#endif /* LESTRA_NET_H */
