/*
 * Lestra OS - Core TCP/IP stack (Ethernet + ARP + IP + ICMP + UDP + DHCP + DNS)
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * This file implements everything below TCP. TCP itself is in tcp.c
 * because it's a stateful beast. HTTP is in http.c.
 *
 * Design:
 *   - Single NIC (E1000 driver in drivers/net/e1000.c).
 *   - Polling-based packet pump: net_tick() is called from the 1 kHz
 *     timer IRQ. Each tick we drain the RX ring and dispatch packets
 *     to the right protocol handler.
 *   - Static single-IP configuration obtained via DHCP at boot.
 *   - No routing table: anything not on our subnet goes to the gateway.
 *
 * We use a single static outgoing pktbuf since the kernel is single-
 * task (no preemption). Same for ARP cache and DNS cache.
 */

#include <lestra/types.h>
#include <lestra/net.h>
#include <lestra/printk.h>
#include <lestra/panic.h>
#include <lestra/timer.h>
#include <string.h>

/* E1000 driver entry points (defined in drivers/net/e1000.c) */
extern int        e1000_init(void);
extern int        e1000_is_present(void);
extern mac_addr_t e1000_get_mac(void);
extern int        e1000_send(const void* data, uint16_t len);
extern int        e1000_recv(void* buf, uint16_t bufsz);

/* ----- Ethernet header ----- */
#define ETH_TYPE_IPV4  0x0800
#define ETH_TYPE_ARP   0x0806

struct eth_hdr {
    mac_addr_t  dst;
    mac_addr_t  src;
    uint16_t    ethertype;
} __packed;

/* ----- ARP ----- */
#define ARP_HW_ETHER  1
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

struct arp_pkt {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t op;
    mac_addr_t  sha;
    ipv4_addr_t spa;
    mac_addr_t  tha;
    ipv4_addr_t tpa;
} __packed;

#define ARP_CACHE_SIZE 8
struct arp_entry {
    ipv4_addr_t ip;
    mac_addr_t  mac;
    uint8_t     valid;
};
static struct arp_entry arp_cache[ARP_CACHE_SIZE];

/* Pending ARP request: we send a request and wait for a reply.
 * While waiting, net_tick() retransmits every 100 ms up to 5 tries. */
static ipv4_addr_t arp_pending_ip = IP_ZERO;
static mac_addr_t  arp_pending_mac = MAC_ZERO;
static int         arp_pending_done = 0;       /* 1 = answered, -1 = timed out */
static uint64_t    arp_pending_started = 0;
static int         arp_pending_tries = 0;

/* ----- IPv4 header ----- */
struct ip_hdr {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    ipv4_addr_t src;
    ipv4_addr_t dst;
} __packed;

#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

/* ----- ICMP ----- */
struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __packed;

/* ----- UDP ----- */
struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;   /* 0 = no checksum (legal for IPv4) */
} __packed;

/* ----- DHCP -----
 * We use a single XID per session. The state machine runs in net_tick(). */
#define DHCP_BOOTREQUEST 1
#define DHCP_BOOTREPLY   2
#define DHCP_MAGIC  0x63825363

#define DHCP_OPT_PAD             0
#define DHCP_OPT_SUBNET_MASK     1
#define DHCP_OPT_ROUTER          3
#define DHCP_OPT_DNS             6
#define DHCP_OPT_REQ_IP          50
#define DHCP_OPT_LEASE_TIME      51
#define DHCP_OPT_MSG_TYPE        53
#define DHCP_OPT_SERVER_ID       54
#define DHCP_OPT_PARAM_LIST      55
#define DHCP_OPT_END             255

#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5

struct dhcp_pkt {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    ipv4_addr_t ciaddr;
    ipv4_addr_t yiaddr;
    ipv4_addr_t siaddr;
    ipv4_addr_t giaddr;
    mac_addr_t  chaddr;
    uint8_t     chaddr_pad[10];
    uint8_t     sname[64];
    uint8_t     file[128];
    uint32_t    magic;
    /* Standard DHCP options field is 312 bytes min (RFC 2131 §3.4).
     * Total packet = 240 fixed + 312 options = 552 bytes. Many DHCP
     * servers reject packets smaller than 300 bytes. */
    uint8_t     options[312];
} __packed;

/* ----- DNS ----- */
#define DNS_PORT 53
#define DNS_MAX_NAME 256

struct dns_hdr {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __packed;

struct dns_question {
    /* name is variable length, so we don't include it here */
    uint16_t qtype;
    uint16_t qclass;
} __packed;

/* ----- stack state ----- */
static int          net_initialized = 0;
static int          net_link_up = 0;
/* my_ip is non-static so tcp.c can read it for checksum computation. */
       ipv4_addr_t  my_ip   = IP_ZERO;
static ipv4_addr_t  my_mask = IP_ZERO;
static ipv4_addr_t  my_gw   = IP_ZERO;
static ipv4_addr_t  my_dns  = IP_ZERO;
static mac_addr_t   my_mac  = MAC_ZERO;

/* DHCP state machine */
typedef enum {
    DHCP_STATE_INIT = 0,
    DHCP_STATE_SELECTING,   /* sent DISCOVER, waiting for OFFER */
    DHCP_STATE_REQUESTING,  /* sent REQUEST, waiting for ACK */
    DHCP_STATE_BOUND,
    DHCP_STATE_FAILED
} dhcp_state_t;
static dhcp_state_t dhcp_state = DHCP_STATE_INIT;
static uint32_t     dhcp_xid = 0;
static ipv4_addr_t  dhcp_offered_ip = IP_ZERO;
static ipv4_addr_t  dhcp_server_id  = IP_ZERO;
static uint64_t     dhcp_last_msg_time = 0;
static int          dhcp_tries = 0;

/* UDP demultiplexing: we have a small set of in-flight UDP exchanges.
 * For simplicity, a single "pending UDP reply" slot: when we send a UDP
 * packet, we record the local/remote port pair. When a UDP packet
 * arrives matching that pair, we copy it into udp_pending_buf and set
 * udp_pending_len. The caller polls udp_pending_len. */
#define UDP_BUF_SZ 1500
static uint8_t    udp_pending_buf[UDP_BUF_SZ];
static uint16_t   udp_pending_len = 0;
static uint16_t   udp_expect_src_port = 0;   /* 0 = no pending */
static uint16_t   udp_expect_dst_port = 0;   /* port we sent from */

/* ----- forward decls ----- */
static void handle_ethernet(uint8_t* data, uint16_t len);
static void handle_arp(uint8_t* data, uint16_t len, mac_addr_t src_mac);
static void handle_ipv4(uint8_t* data, uint16_t len);
static void handle_icmp(struct ip_hdr* ip, uint8_t* data, uint16_t len);
static void handle_udp(struct ip_hdr* ip, uint8_t* data, uint16_t len);
static void dhcp_start(void);
static void dhcp_tick(void);
static void dhcp_handle(struct ip_hdr* ip, uint8_t* data, uint16_t len);

/* TCP entry (defined in tcp.c) */
extern void tcp_handle(struct ip_hdr* ip, uint8_t* data, uint16_t len);
extern void tcp_tick(void);

/* HTTP server (defined in http_server.c) */
extern void http_server_tick(void);

/* ----- checksum (Internet 16-bit one's complement) ----- */
static uint16_t inet_checksum(const void* data, uint16_t len, uint32_t init_sum) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t sum = init_sum;
    for (uint16_t i = 0; i + 1 < len; i += 2) {
        sum += ((uint16_t)p[i] << 8) | p[i+1];
    }
    if (len & 1) {
        sum += (uint16_t)p[len-1] << 8;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static uint16_t ip_checksum(const void* data, uint16_t len) {
    return inet_checksum(data, len, 0);
}

/* pseudo-header for UDP/TCP checksums */
static uint16_t l4_pseudo_checksum(ipv4_addr_t src, ipv4_addr_t dst,
                                    uint8_t proto,
                                    const void* l4, uint16_t l4_len) {
    uint32_t sum = 0;
    sum += (src.bytes[0] << 8) | src.bytes[1];
    sum += (src.bytes[2] << 8) | src.bytes[3];
    sum += (dst.bytes[0] << 8) | dst.bytes[1];
    sum += (dst.bytes[2] << 8) | dst.bytes[3];
    sum += proto;
    sum += l4_len;
    return inet_checksum(l4, l4_len, sum);
}

/* ----- ARP cache ----- */
static mac_addr_t arp_lookup(ipv4_addr_t ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && ipv4_eq(arp_cache[i].ip, ip)) {
            return arp_cache[i].mac;
        }
    }
    return MAC_ZERO;
}

static void arp_cache_add(ipv4_addr_t ip, mac_addr_t mac) {
    /* Don't cache 0.0.0.0 or the broadcast MAC */
    if (ipv4_is_zero(ip)) return;
    /* Find existing entry or empty slot */
    int empty = -1;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) { empty = i; break; }
        if (ipv4_eq(arp_cache[i].ip, ip)) {
            arp_cache[i].mac = mac;
            arp_cache[i].valid = 1;
            return;
        }
    }
    if (empty >= 0) {
        arp_cache[empty].ip = ip;
        arp_cache[empty].mac = mac;
        arp_cache[empty].valid = 1;
    }
    /* else: cache full, just drop (caller will retry ARP) */
}

/* Send an ARP request for `ip`. */
static void arp_send_request(ipv4_addr_t ip) {
    uint8_t buf[sizeof(struct eth_hdr) + sizeof(struct arp_pkt)];
    struct eth_hdr*  eth = (struct eth_hdr*)&buf[0];
    struct arp_pkt*  arp = (struct arp_pkt*)&buf[sizeof(struct eth_hdr)];

    eth->dst = MAC_BROADCAST;
    eth->src = my_mac;
    eth->ethertype = htons16(ETH_TYPE_ARP);

    arp->htype = htons16(ARP_HW_ETHER);
    arp->ptype = htons16(ETH_TYPE_IPV4);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->op    = htons16(ARP_OP_REQUEST);
    arp->sha   = my_mac;
    arp->spa   = my_ip;
    arp->tha   = MAC_ZERO;
    arp->tpa   = ip;

    e1000_send(buf, sizeof(buf));
}

/* Public: resolve an IP to a MAC. Blocks up to `timeout_ms` ms. */
static mac_addr_t arp_resolve(ipv4_addr_t ip, uint32_t timeout_ms) {
    /* Check cache first */
    mac_addr_t m = arp_lookup(ip);
    if (m.bytes[0] || m.bytes[1] || m.bytes[2] ||
        m.bytes[3] || m.bytes[4] || m.bytes[5]) {
        return m;
    }
    /* Don't ARP for ourselves or for broadcast */
    if (ipv4_eq(ip, my_ip)) return my_mac;
    if (ipv4_eq(ip, IP_BROADCAST)) return MAC_BROADCAST;

    /* Send request and wait */
    arp_pending_ip = ip;
    arp_pending_mac = MAC_ZERO;
    arp_pending_done = 0;
    arp_pending_started = timer_get_ms();
    arp_pending_tries = 0;

    arp_send_request(ip);

    uint64_t deadline = arp_pending_started + timeout_ms;
    while (timer_get_ms() < deadline) {
        if (arp_pending_done == 1) {
            return arp_pending_mac;
        }
        /* Retransmit every 200 ms */
        if (timer_get_ms() > arp_pending_started + (uint64_t)(arp_pending_tries + 1) * 200) {
            arp_pending_tries++;
            arp_send_request(ip);
        }
        /* Pump the stack while we wait */
        net_tick();
    }
    arp_pending_done = -1;
    return MAC_ZERO;
}

/* ----- Ethernet send ----- */
/* Send an IPv4 packet to `dst_ip`. Handles gateway routing and ARP. */
static int eth_send_ipv4(ipv4_addr_t dst_ip, uint8_t proto,
                          const void* payload, uint16_t payload_len) {
    if (!net_link_up) return 0;

    /* Pick next hop: if dst is on our subnet, send direct; else use gateway. */
    ipv4_addr_t next_hop = dst_ip;
    /* Compute subnet broadcast: if (ip & mask) != (dst & mask), use gateway */
    uint32_t my_net  = ((uint32_t)my_ip.bytes[0] << 24) | ((uint32_t)my_ip.bytes[1] << 16)
                     | ((uint32_t)my_ip.bytes[2] << 8) | my_ip.bytes[3];
    uint32_t dst_net = ((uint32_t)dst_ip.bytes[0] << 24) | ((uint32_t)dst_ip.bytes[1] << 16)
                     | ((uint32_t)dst_ip.bytes[2] << 8) | dst_ip.bytes[3];
    uint32_t mask    = ((uint32_t)my_mask.bytes[0] << 24) | ((uint32_t)my_mask.bytes[1] << 16)
                     | ((uint32_t)my_mask.bytes[2] << 8) | my_mask.bytes[3];
    if ((my_net & mask) != (dst_net & mask)) {
        /* Off subnet: route via gateway */
        if (!ipv4_is_zero(my_gw)) {
            next_hop = my_gw;
        }
    }

    mac_addr_t dst_mac = arp_resolve(next_hop, 1000);
    /* If ARP failed, bail out */
    int mac_any = dst_mac.bytes[0] | dst_mac.bytes[1] | dst_mac.bytes[2]
                | dst_mac.bytes[3] | dst_mac.bytes[4] | dst_mac.bytes[5];
    if (!mac_any && !ipv4_eq(next_hop, IP_BROADCAST)) {
        pr_warn("net: ARP failed for %u.%u.%u.%u\n",
                next_hop.bytes[0], next_hop.bytes[1], next_hop.bytes[2], next_hop.bytes[3]);
        return 0;
    }

    /* Build the packet: ETH + IP + payload */
    uint16_t total = sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + payload_len;
    if (total > NET_MAX_PKT) return 0;

    static uint8_t buf[NET_MAX_PKT];
    struct eth_hdr*  eth = (struct eth_hdr*)&buf[0];
    struct ip_hdr*   ip  = (struct ip_hdr*)&buf[sizeof(struct eth_hdr)];
    uint8_t*         pl  = &buf[sizeof(struct eth_hdr) + sizeof(struct ip_hdr)];

    eth->dst = dst_mac;
    eth->src = my_mac;
    eth->ethertype = htons16(ETH_TYPE_IPV4);

    ip->ver_ihl  = 0x45;     /* IPv4, IHL=5 (20 bytes) */
    ip->tos      = 0;
    ip->total_len = htons16(sizeof(struct ip_hdr) + payload_len);
    ip->id       = htons16(0x1234);  /* good enough for our single-packet use */
    ip->flags_frag = htons16(0x4000); /* Don't Fragment */
    ip->ttl      = 64;
    ip->proto    = proto;
    ip->checksum = 0;
    ip->src      = my_ip;
    ip->dst      = dst_ip;
    /* ip_checksum returns a value in HOST byte order (the one's-complement
     * of the sum). The wire format expects network (big-endian) byte
     * order, so we must htons16 it before storing into the struct field
     * (which on x86_64 is little-endian). */
    ip->checksum = htons16(ip_checksum(ip, sizeof(struct ip_hdr)));

    memcpy(pl, payload, payload_len);

    return e1000_send(buf, total);
}

/* ----- ICMP (ping) ----- */
static void handle_icmp(struct ip_hdr* ip, uint8_t* data, uint16_t len) {
    if (len < sizeof(struct icmp_hdr)) return;
    struct icmp_hdr* icmp = (struct icmp_hdr*)data;
    if (icmp->type == 8) {  /* Echo Request - reply */
        /* Build Echo Reply: same payload, type=0 */
        icmp->type = 0;
        icmp->checksum = 0;
        icmp->checksum = htons16(ip_checksum(icmp, len));
        eth_send_ipv4(ip->src, IP_PROTO_ICMP, data, len);
    }
    /* Echo Reply handling (for our own pings) is done in net_ping() */
}

int net_ping(ipv4_addr_t target, uint16_t seq, uint32_t timeout_ms) {
    if (!net_link_up) return 0;

    /* Build ICMP Echo Request */
    uint8_t buf[sizeof(struct icmp_hdr) + 32];
    struct icmp_hdr* icmp = (struct icmp_hdr*)buf;
    icmp->type = 8;   /* Echo Request */
    icmp->code = 0;
    icmp->id   = htons16(0xBEEF);
    icmp->seq  = htons16(seq);
    /* Payload: fill with 0x00 padding */
    for (int i = 0; i < 32; i++) buf[sizeof(struct icmp_hdr) + i] = 0;
    icmp->checksum = 0;
    icmp->checksum = htons16(ip_checksum(buf, sizeof(buf)));

    /* Save expected id/seq for matching replies */
    extern uint16_t net_ping_expect_id;
    extern uint16_t net_ping_expect_seq;
    extern int      net_ping_got_reply;
    net_ping_expect_id = ntohs16(icmp->id);
    net_ping_expect_seq = ntohs16(icmp->seq);
    net_ping_got_reply = 0;

    if (!eth_send_ipv4(target, IP_PROTO_ICMP, buf, sizeof(buf))) {
        return 0;
    }

    /* Wait for reply */
    uint64_t deadline = timer_get_ms() + timeout_ms;
    while (timer_get_ms() < deadline) {
        if (net_ping_got_reply) {
            return 1;
        }
        net_tick();
    }
    return 0;
}

/* ping reply state - referenced by net_ping() above */
uint16_t net_ping_expect_id = 0;
uint16_t net_ping_expect_seq = 0;
int      net_ping_got_reply = 0;

/* ----- UDP ----- */
/* Send a UDP datagram. Returns bytes sent (header+payload) or 0 on failure. */
int udp_send(ipv4_addr_t dst_ip, uint16_t src_port, uint16_t dst_port,
              const void* payload, uint16_t payload_len) {
    uint8_t buf[sizeof(struct udp_hdr) + 1500];
    struct udp_hdr* u = (struct udp_hdr*)buf;
    u->src_port = htons16(src_port);
    u->dst_port = htons16(dst_port);
    u->length   = htons16(sizeof(struct udp_hdr) + payload_len);
    u->checksum = 0;  /* 0 = no checksum (legal for IPv4) */
    memcpy(&buf[sizeof(struct udp_hdr)], payload, payload_len);
    uint16_t total = sizeof(struct udp_hdr) + payload_len;

    /* Record the port pair so the RX path can match the reply */
    udp_expect_src_port = dst_port;   /* we expect a reply FROM the port we sent TO */
    udp_expect_dst_port = src_port;   /* ...arriving AT the port we sent FROM */
    udp_pending_len = 0;

    if (!eth_send_ipv4(dst_ip, IP_PROTO_UDP, buf, total)) {
        return 0;
    }
    return total;
}

static void handle_udp(struct ip_hdr* ip, uint8_t* data, uint16_t len) {
    if (len < sizeof(struct udp_hdr)) return;
    struct udp_hdr* u = (struct udp_hdr*)data;

    /* If this is a DHCP reply (port 68), route to DHCP */
    uint16_t dst_port = ntohs16(u->dst_port);
    uint16_t src_port = ntohs16(u->src_port);
    if (dst_port == 68 && src_port == 67) {
        dhcp_handle(ip, &data[sizeof(struct udp_hdr)],
                    len - sizeof(struct udp_hdr));
        return;
    }

    /* If we have a pending UDP exchange and this matches, capture it */
    if (udp_expect_src_port != 0 &&
        src_port == udp_expect_src_port &&
        dst_port == udp_expect_dst_port) {
        uint16_t plen = len - sizeof(struct udp_hdr);
        if (plen > UDP_BUF_SZ) plen = UDP_BUF_SZ;
        memcpy(udp_pending_buf, &data[sizeof(struct udp_hdr)], plen);
        udp_pending_len = plen;
        udp_expect_src_port = 0;
        return;
    }
    /* Otherwise drop */
}

/* Public helper: wait for a UDP reply with timeout */
int udp_recv_wait(uint8_t* buf, uint16_t bufsz, uint32_t timeout_ms) {
    uint64_t deadline = timer_get_ms() + timeout_ms;
    while (timer_get_ms() < deadline) {
        if (udp_pending_len > 0) {
            uint16_t n = udp_pending_len;
            if (n > bufsz) n = bufsz;
            memcpy(buf, udp_pending_buf, n);
            udp_pending_len = 0;
            return n;
        }
        net_tick();
    }
    return 0;
}

/* ----- DHCP ----- */
static void dhcp_add_option(uint8_t** p, uint8_t code, uint8_t len, const void* data) {
    (*p)[0] = code;
    (*p)[1] = len;
    memcpy(&(*p)[2], data, len);
    *p += 2 + len;
}

static void dhcp_send(uint8_t msg_type, ipv4_addr_t requested_ip, ipv4_addr_t server_id) {
    static uint8_t buf[sizeof(struct eth_hdr) + sizeof(struct ip_hdr) +
                       sizeof(struct udp_hdr) + sizeof(struct dhcp_pkt)];
    struct eth_hdr*  eth = (struct eth_hdr*)&buf[0];
    struct ip_hdr*   ip  = (struct ip_hdr*)&buf[sizeof(struct eth_hdr)];
    struct udp_hdr*  u   = (struct udp_hdr*)&buf[sizeof(struct eth_hdr) + sizeof(struct ip_hdr)];
    struct dhcp_pkt* d   = (struct dhcp_pkt*)&buf[sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + sizeof(struct udp_hdr)];

    /* Broadcast Ethernet frame for DISCOVER; unicast to server for REQUEST */
    int broadcast = (msg_type == DHCP_MSG_DISCOVER);
    mac_addr_t dst_mac = broadcast ? MAC_BROADCAST : arp_lookup(server_id);
    if (broadcast) {
        eth->dst = MAC_BROADCAST;
    } else {
        eth->dst = dst_mac;
        /* If we don't have server MAC, broadcast anyway */
        int any = dst_mac.bytes[0] | dst_mac.bytes[1] | dst_mac.bytes[2]
                | dst_mac.bytes[3] | dst_mac.bytes[4] | dst_mac.bytes[5];
        if (!any) eth->dst = MAC_BROADCAST;
    }
    eth->src = my_mac;
    eth->ethertype = htons16(ETH_TYPE_IPV4);

    ip->ver_ihl = 0x45;
    ip->tos = 0;
    uint16_t ip_total = sizeof(struct ip_hdr) + sizeof(struct udp_hdr) + sizeof(struct dhcp_pkt);
    ip->total_len = htons16(ip_total);
    ip->id = htons16(0x1234);
    ip->flags_frag = htons16(0x4000);
    ip->ttl = 64;
    ip->proto = IP_PROTO_UDP;
    ip->checksum = 0;
    ip->src = IP_ZERO;  /* 0.0.0.0 during DHCP */
    ip->dst = broadcast ? IP_BROADCAST : server_id;
    ip->checksum = htons16(ip_checksum(ip, sizeof(struct ip_hdr)));

    u->src_port = htons16(68);  /* DHCP client port */
    u->dst_port = htons16(67);  /* DHCP server port */
    u->length   = htons16(sizeof(struct udp_hdr) + sizeof(struct dhcp_pkt));
    u->checksum = 0;

    memset(d, 0, sizeof(*d));
    d->op = DHCP_BOOTREQUEST;
    d->htype = 1;   /* Ethernet */
    d->hlen  = 6;
    d->xid   = dhcp_xid;
    d->flags = htons16(0x8000);  /* broadcast reply requested */
    d->chaddr = my_mac;
    d->magic = htonl32(DHCP_MAGIC);

    /* Options */
    uint8_t* p = d->options;
    /* FIX: was hardcoding msg_type=1 (DISCOVER) for all packets. Now
     * uses the msg_type parameter passed in by the caller. */
    dhcp_add_option(&p, DHCP_OPT_MSG_TYPE, 1, &msg_type);
    if (!ipv4_is_zero(requested_ip)) {
        dhcp_add_option(&p, DHCP_OPT_REQ_IP, 4, &requested_ip);
    }
    if (!ipv4_is_zero(server_id)) {
        dhcp_add_option(&p, DHCP_OPT_SERVER_ID, 4, &server_id);
    }
    if (msg_type == DHCP_MSG_DISCOVER) {
        /* Parameter request list: subnet mask, router, DNS, lease time */
        uint8_t params[] = { DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER, DHCP_OPT_DNS, DHCP_OPT_LEASE_TIME };
        dhcp_add_option(&p, DHCP_OPT_PARAM_LIST, sizeof(params), params);
    }
    *p++ = DHCP_OPT_END;

    /* Send via the driver directly (we built the full Ethernet frame) */
    e1000_send(buf, sizeof(buf));
}

static void dhcp_start(void) {
    dhcp_xid = 0x12345678u ^ (uint32_t)timer_get_ms();
    dhcp_state = DHCP_STATE_SELECTING;
    dhcp_last_msg_time = 0;   /* will trigger immediate send in dhcp_tick */
    dhcp_tries = 0;
    pr_info("DHCP: starting (xid=0x%x)\n", (unsigned)dhcp_xid);
}

static void dhcp_handle(struct ip_hdr* ip, uint8_t* data, uint16_t len) {
    /* We need at least the fixed BOOTP header + magic (240 bytes).
     * The options field can be shorter than our struct's 312-byte
     * declaration — servers are free to send fewer options. */
    if (len < 240) return;
    struct dhcp_pkt* d = (struct dhcp_pkt*)data;
    if (d->xid != dhcp_xid) return;  /* not for us */
    if (ntohl32(d->magic) != DHCP_MAGIC) return;

    /* Find message type option */
    uint8_t msg_type = 0;
    ipv4_addr_t subnet = IP_ZERO, router = IP_ZERO, dns = IP_ZERO;
    ipv4_addr_t server_id = IP_ZERO;
    uint8_t* opt = d->options;
    uint8_t* end = (uint8_t*)data + len;
    while (opt < end && *opt != DHCP_OPT_END) {
        if (*opt == DHCP_OPT_PAD) { opt++; continue; }
        uint8_t code = *opt++;
        uint8_t olen = *opt++;
        if (opt + olen > end) break;
        if (code == DHCP_OPT_MSG_TYPE && olen >= 1) {
            msg_type = opt[0];
        } else if (code == DHCP_OPT_SUBNET_MASK && olen >= 4) {
            memcpy(&subnet, opt, 4);
        } else if (code == DHCP_OPT_ROUTER && olen >= 4) {
            memcpy(&router, opt, 4);
        } else if (code == DHCP_OPT_DNS && olen >= 4) {
            memcpy(&dns, opt, 4);
        } else if (code == DHCP_OPT_SERVER_ID && olen >= 4) {
            memcpy(&server_id, opt, 4);
        }
        opt += olen;
    }

    if (msg_type == DHCP_MSG_OFFER && dhcp_state == DHCP_STATE_SELECTING) {
        dhcp_offered_ip = d->yiaddr;
        dhcp_server_id  = server_id;
        pr_info("DHCP: OFFER ip=%u.%u.%u.%u from server %u.%u.%u.%u\n",
                d->yiaddr.bytes[0], d->yiaddr.bytes[1], d->yiaddr.bytes[2], d->yiaddr.bytes[3],
                server_id.bytes[0], server_id.bytes[1], server_id.bytes[2], server_id.bytes[3]);
        /* Transition to REQUESTING: send REQUEST in next tick */
        dhcp_state = DHCP_STATE_REQUESTING;
        dhcp_last_msg_time = 0;
        dhcp_tries = 0;
    } else if (msg_type == DHCP_MSG_ACK && dhcp_state == DHCP_STATE_REQUESTING) {
        my_ip   = d->yiaddr;
        my_mask = subnet;
        my_gw   = router;
        my_dns  = dns;
        dhcp_state = DHCP_STATE_BOUND;
        pr_info("DHCP: ACK - configured ip=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u\n",
                my_ip.bytes[0], my_ip.bytes[1], my_ip.bytes[2], my_ip.bytes[3],
                my_mask.bytes[0], my_mask.bytes[1], my_mask.bytes[2], my_mask.bytes[3],
                my_gw.bytes[0], my_gw.bytes[1], my_gw.bytes[2], my_gw.bytes[3],
                my_dns.bytes[0], my_dns.bytes[1], my_dns.bytes[2], my_dns.bytes[3]);
        /* Cache the gateway's MAC since we'll need it for any off-subnet traffic */
        if (!ipv4_is_zero(my_gw)) {
            /* Trigger ARP for gateway in background (best-effort) */
            arp_send_request(my_gw);
        }
    }
}

static void dhcp_tick(void) {
    if (dhcp_state == DHCP_STATE_BOUND || dhcp_state == DHCP_STATE_FAILED) return;

    uint64_t now = timer_get_ms();
    /* Send the next message if it's been > 1 second since the last one */
    if (now - dhcp_last_msg_time < 1000) return;
    dhcp_last_msg_time = now;
    dhcp_tries++;

    if (dhcp_tries > 10) {
        pr_warn("DHCP: timed out after 10 tries\n");
        dhcp_state = DHCP_STATE_FAILED;
        return;
    }

    if (dhcp_state == DHCP_STATE_SELECTING) {
        pr_info("DHCP: sending DISCOVER (try %d)\n", dhcp_tries);
        dhcp_send(DHCP_MSG_DISCOVER, IP_ZERO, IP_ZERO);
    } else if (dhcp_state == DHCP_STATE_REQUESTING) {
        pr_info("DHCP: sending REQUEST for %u.%u.%u.%u (try %d)\n",
                dhcp_offered_ip.bytes[0], dhcp_offered_ip.bytes[1],
                dhcp_offered_ip.bytes[2], dhcp_offered_ip.bytes[3],
                dhcp_tries);
        dhcp_send(DHCP_MSG_REQUEST, dhcp_offered_ip, dhcp_server_id);
    }
}

/* ----- DNS ----- */
/* Encode "host.example.com" into DNS label format: \x04host\x07example\x03com\x00 */
static int dns_encode_name(uint8_t* out, int outsz, const char* name) {
    int outlen = 0;
    const char* p = name;
    while (*p) {
        const char* dot = p;
        while (*dot && *dot != '.') dot++;
        int labellen = dot - p;
        if (labellen > 63 || labellen == 0) return -1;
        if (outlen + 1 + labellen > outsz - 1) return -1;
        out[outlen++] = (uint8_t)labellen;
        for (int i = 0; i < labellen; i++) {
            out[outlen++] = (uint8_t)p[i];
        }
        p = dot;
        if (*p == '.') p++;
    }
    out[outlen++] = 0;   /* root label */
    return outlen;
}

/* Decode a DNS name, following compression pointers. Returns total
 * bytes consumed from `data` (the original packet). */
static int dns_decode_name(const uint8_t* data, uint16_t datalen,
                            const uint8_t* p, char* out, int outsz) {
    int outlen = 0;
    int followed_ptr = 0;
    int first_consumed = 0;
    int total_consumed = 0;

    while (1) {
        if (p < data || p >= data + datalen) return -1;
        uint8_t len = *p;
        if (len == 0) {
            if (!followed_ptr) total_consumed = (p - data) + 1;
            if (outlen == 0) { if (outsz > 0) out[0] = '\0'; }
            else { if (outlen < outsz) out[outlen] = '\0'; }
            return total_consumed;
        }
        if ((len & 0xC0) == 0xC0) {
            /* compression pointer */
            if (p + 1 >= data + datalen) return -1;
            uint16_t off = ((len & 0x3F) << 8) | p[1];
            if (!followed_ptr) {
                total_consumed = (p - data) + 2;
                first_consumed = total_consumed;
            }
            followed_ptr = 1;
            if (off >= datalen) return -1;
            p = data + off;
            continue;
        }
        /* regular label */
        p++;
        for (int i = 0; i < len; i++) {
            if (p + i >= data + datalen) return -1;
            if (outlen + 1 < outsz) out[outlen++] = (char)p[i];
        }
        p += len;
        if (outlen + 1 < outsz) out[outlen++] = '.';
    }
}

int net_resolve(const char* hostname, ipv4_addr_t* out) {
    if (!net_link_up) return 0;
    if (ipv4_is_zero(my_dns)) {
        pr_warn("DNS: no DNS server known\n");
        return 0;
    }

    /* Adblock check: if the hostname matches a blocked domain suffix,
     * return 0.0.0.0 so the connection fails immediately. */
    extern int adblock_should_block(const char* hostname);
    if (adblock_should_block(hostname)) {
        pr_info("adblock: blocked DNS lookup for %s\n", hostname);
        out->bytes[0] = 0; out->bytes[1] = 0;
        out->bytes[2] = 0; out->bytes[3] = 0;
        return 1;  /* "success" but with 0.0.0.0 — caller will fail to connect */
    }

    /* If hostname is already an IPv4 dotted-quad, parse it directly. */
    if (hostname[0] >= '0' && hostname[0] <= '9') {
        int n = 0;
        const char* s = hostname;
        int vals[4] = {0,0,0,0};
        int vi = 0;
        while (*s && vi < 4) {
            if (*s >= '0' && *s <= '9') {
                vals[vi] = vals[vi] * 10 + (*s - '0');
                s++;
            } else if (*s == '.') {
                vi++;
                s++;
            } else {
                n = -1; break;
            }
        }
        if (n == 0 && vi == 3 && !*s) {
            out->bytes[0] = vals[0]; out->bytes[1] = vals[1];
            out->bytes[2] = vals[2]; out->bytes[3] = vals[3];
            return 1;
        }
    }

    /* Build DNS query */
    static uint8_t qbuf[512];
    struct dns_hdr* hdr = (struct dns_hdr*)qbuf;
    memset(hdr, 0, sizeof(*hdr));
    hdr->id = htons16(0x4242);
    hdr->flags = htons16(0x0100);   /* recursion desired */
    hdr->qdcount = htons16(1);

    uint8_t* p = &qbuf[sizeof(struct dns_hdr)];
    int namelen = dns_encode_name(p, sizeof(qbuf) - sizeof(struct dns_hdr) - 4, hostname);
    if (namelen < 0) return 0;
    p += namelen;
    /* qtype=A (1), qclass=IN (1) */
    *p++ = 0; *p++ = 1;
    *p++ = 0; *p++ = 1;

    uint16_t qlen = (uint16_t)(p - qbuf);

    if (!udp_send(my_dns, 12345, DNS_PORT, qbuf, qlen)) {
        pr_warn("DNS: failed to send query\n");
        return 0;
    }

    /* Wait for reply */
    static uint8_t rbuf[1500];
    int rlen = udp_recv_wait(rbuf, sizeof(rbuf), 3000);
    if (rlen < (int)sizeof(struct dns_hdr)) return 0;
    struct dns_hdr* rhdr = (struct dns_hdr*)rbuf;
    if (rhdr->id != htons16(0x4242)) return 0;
    uint16_t ancount = ntohs16(rhdr->ancount);
    if (ancount == 0) return 0;

    /* Skip the question section */
    uint8_t* q = &rbuf[sizeof(struct dns_hdr)];
    char tmp[DNS_MAX_NAME];
    int consumed = dns_decode_name(rbuf, rlen, q, tmp, sizeof(tmp));
    if (consumed < 0) return 0;
    q += consumed;
    q += 4;  /* qtype + qclass */

    /* Walk the answers */
    for (int i = 0; i < (int)ancount; i++) {
        consumed = dns_decode_name(rbuf, rlen, q, tmp, sizeof(tmp));
        if (consumed < 0) return 0;
        q += consumed;
        if (q + 10 > rbuf + rlen) return 0;
        uint16_t qtype = (q[0] << 8) | q[1];
        uint16_t rdlen = (q[8] << 8) | q[9];
        q += 10;
        if (qtype == 1 && rdlen == 4) {  /* A record */
            if (q + 4 > rbuf + rlen) return 0;
            out->bytes[0] = q[0];
            out->bytes[1] = q[1];
            out->bytes[2] = q[2];
            out->bytes[3] = q[3];
            return 1;
        }
        q += rdlen;
    }
    return 0;
}

/* ----- packet dispatch ----- */
static void handle_arp(uint8_t* data, uint16_t len, mac_addr_t src_mac) {
    if (len < sizeof(struct arp_pkt)) return;
    struct arp_pkt* arp = (struct arp_pkt*)data;
    if (ntohs16(arp->htype) != ARP_HW_ETHER) return;
    if (ntohs16(arp->ptype) != ETH_TYPE_IPV4) return;

    /* Cache the sender's mapping */
    arp_cache_add(arp->spa, arp->sha);

    /* Is this a reply to our pending request? */
    if (ntohs16(arp->op) == ARP_OP_REPLY && ipv4_eq(arp->spa, arp_pending_ip)) {
        arp_pending_mac = arp->sha;
        arp_pending_done = 1;
        return;
    }

    /* Is this a request for our IP? Reply. */
    if (ntohs16(arp->op) == ARP_OP_REQUEST && ipv4_eq(arp->tpa, my_ip)) {
        /* Build reply */
        uint8_t buf[sizeof(struct eth_hdr) + sizeof(struct arp_pkt)];
        struct eth_hdr*  eth = (struct eth_hdr*)&buf[0];
        struct arp_pkt*  rep = (struct arp_pkt*)&buf[sizeof(struct eth_hdr)];
        eth->dst = src_mac;
        eth->src = my_mac;
        eth->ethertype = htons16(ETH_TYPE_ARP);
        rep->htype = arp->htype;
        rep->ptype = arp->ptype;
        rep->hlen  = 6;
        rep->plen  = 4;
        rep->op    = htons16(ARP_OP_REPLY);
        rep->sha   = my_mac;
        rep->spa   = my_ip;
        rep->tha   = arp->sha;
        rep->tpa   = arp->spa;
        e1000_send(buf, sizeof(buf));
    }
}

static void handle_ipv4(uint8_t* data, uint16_t len) {
    if (len < sizeof(struct ip_hdr)) return;
    struct ip_hdr* ip = (struct ip_hdr*)data;
    uint8_t ihl = ip->ver_ihl & 0x0F;
    if (ihl < 5) return;
    uint16_t ip_hdr_len = ihl * 4;
    if (ip_hdr_len > len) return;

    /* Verify it's destined for us (or broadcast) */
    int for_us = ipv4_eq(ip->dst, my_ip) || ipv4_eq(ip->dst, IP_BROADCAST);
    if (!for_us) return;

    uint8_t* l4 = &data[ip_hdr_len];
    uint16_t l4_len = len - ip_hdr_len;

    switch (ip->proto) {
        case IP_PROTO_ICMP:
            handle_icmp(ip, l4, l4_len);
            break;
        case IP_PROTO_UDP:
            handle_udp(ip, l4, l4_len);
            break;
        case IP_PROTO_TCP:
            tcp_handle(ip, l4, l4_len);
            break;
    }
}

static void handle_ethernet(uint8_t* data, uint16_t len) {
    if (len < sizeof(struct eth_hdr)) return;
    struct eth_hdr* eth = (struct eth_hdr*)data;
    uint16_t ethertype = ntohs16(eth->ethertype);
    uint16_t payload_len = len - sizeof(struct eth_hdr);
    uint8_t* payload = &data[sizeof(struct eth_hdr)];

    switch (ethertype) {
        case ETH_TYPE_ARP:
            handle_arp(payload, payload_len, eth->src);
            break;
        case ETH_TYPE_IPV4:
            handle_ipv4(payload, payload_len);
            break;
        /* ignore other protocols */
    }
}

/* ----- public API ----- */
void net_init(void) {
    pr_info("net: initializing network stack\n");
    if (!e1000_init()) {
        pr_warn("net: no NIC - networking disabled\n");
        return;
    }
    my_mac = e1000_get_mac();
    net_initialized = 1;
    net_link_up = 1;   /* link is up from driver perspective; IP not yet */
    dhcp_start();
    /* Apply static network config if set (overrides DHCP) */
    extern void net_config_apply(void);
    net_config_apply();
}

void net_tick(void) {
    if (!net_initialized) return;
    /* Re-entrancy guard: net_tick() is normally called from the timer IRQ,
     * but TCP/DHCP/ARP wait loops also call it synchronously while waiting
     * for replies. Without this guard, an IRQ firing during a synchronous
     * wait would re-enter net_tick() and corrupt the static RX buffer +
     * DHCP/TCP state machines. */
    static int in_net_tick = 0;
    if (in_net_tick) return;
    in_net_tick = 1;

    /* Drain RX ring - bounded per tick so we don't starve IRQ0. */
    static uint8_t buf[NET_MAX_PKT];
    int rx_count = 0;
    while (rx_count < 8) {
        int n = e1000_recv(buf, sizeof(buf));
        if (n <= 0) break;
        rx_count++;
        /* Check for ICMP Echo Reply (for our own pings). */
        if (n >= (int)sizeof(struct eth_hdr) + (int)sizeof(struct ip_hdr)) {
            struct eth_hdr* eth = (struct eth_hdr*)buf;
            if (ntohs16(eth->ethertype) == ETH_TYPE_IPV4) {
                struct ip_hdr* ip = (struct ip_hdr*)&buf[sizeof(struct eth_hdr)];
                if (ip->proto == IP_PROTO_ICMP) {
                    uint8_t ihl = ip->ver_ihl & 0x0F;
                    struct icmp_hdr* icmp = (struct icmp_hdr*)&buf[sizeof(struct eth_hdr) + ihl*4];
                    if (icmp->type == 0 &&
                        ntohs16(icmp->id) == net_ping_expect_id &&
                        ntohs16(icmp->seq) == net_ping_expect_seq) {
                        net_ping_got_reply = 1;
                        continue;
                    }
                }
            }
        }
        /* Otherwise dispatch to the right protocol handler. */
        handle_ethernet(buf, (uint16_t)n);
    }
    /* Advance DHCP and TCP state machines. */
    dhcp_tick();
    tcp_tick();
    http_server_tick();

    in_net_tick = 0;
}

int net_is_up(void) {
    if (!net_link_up) return 0;
    if (dhcp_state == DHCP_STATE_BOUND) return 1;
    /* Static config also counts as "up" */
    extern int net_config_is_static(void);
    return net_config_is_static() && !ipv4_is_zero(my_ip);
}
ipv4_addr_t  net_get_ip(void)        { return my_ip; }
ipv4_addr_t  net_get_gateway(void)   { return my_gw; }
ipv4_addr_t  net_get_dns(void)       { return my_dns; }
mac_addr_t   net_get_mac(void)       { return my_mac; }
const char*  net_get_iface_name(void){ return "e1000"; }

/* Expose eth_send_ipv4 and udp_send to TCP and HTTP layers */
int eth_send_ipv4_pub(ipv4_addr_t dst_ip, uint8_t proto,
                       const void* payload, uint16_t payload_len) {
    return eth_send_ipv4(dst_ip, proto, payload, payload_len);
}

/* Setter functions for net_config module (static IP override) */
void net_set_mask(ipv4_addr_t mask) { my_mask = mask; }
void net_set_gw(ipv4_addr_t gw)     { my_gw = gw; }
void net_set_dns(ipv4_addr_t dns)   { my_dns = dns; }
