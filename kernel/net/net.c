/*
 * Lestra OS - Core TCP/IP stack (Ethernet + ARP + IP + ICMP + UDP + DHCP + DNS)
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * This file implements everything below TCP. TCP itself is in tcp.c
 * because it's a stateful beast. HTTP is in http.c.
 *
 * Design:
 *   - Single NIC: VirtIO-net (preferred on KVM/QEMU VPS) or E1000 (Intel).
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
#include <lestra/nic.h>
#include <lestra/firewall.h>
#include <lestra/printk.h>
#include <lestra/panic.h>
#include <lestra/timer.h>
#include <string.h>

/* NIC driver table and active pointer (defined in net/nic.c) */
extern const struct nic_ops *const nic_driver_table[];
extern const int              nic_driver_count;
extern const struct nic_ops *active_nic_ops;

/* Unified driver wrappers — dispatch through active NIC vtable */
static int net_driver_send(const void* data, uint16_t len) {
    if (!active_nic_ops) return -1;
    return active_nic_ops->send(data, len);
}
static int net_driver_recv(void* buf, uint16_t bufsz) {
    if (!active_nic_ops) return 0;
    return active_nic_ops->recv(buf, bufsz);
}

/* WiFi frame handler (defined in net/wifi.c) */
extern void wifi_handle_frame(const uint8_t* data, uint16_t len);
extern void wifi_handle_eapol_frame(const uint8_t* data, uint16_t len);

/* ----- Ethernet header ----- */
#define ETH_TYPE_IPV4       0x0800
#define ETH_TYPE_ARP        0x0806
#define ETH_TYPE_IPV6       0x86DD
#define ETH_TYPE_WLAN_MGMT  0x88B4  /* 802.11 management frames over Ethernet */
#define ETH_TYPE_EAPOL      0x888E  /* EAPOL (WPA2 4-way handshake) */

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
/* NIC driver type ("virtio_net" or "e1000") for net_get_iface_name() */
static const char* net_iface_name = "e1000";

/* IPv6 state */
static ipv6_addr_t  my_ip6       = IPV6_ZERO;
static ipv6_addr_t  my_ip6_gw    = IPV6_ZERO;
static int          ipv6_valid   = 0;   /* 1 once we have a global address */
static int          ipv6_auto_done = 0;

/* NDP cache */
static struct ndp_cache_entry ndp_cache[NDP_CACHE_SIZE];

/* Pending NDP resolution */
static ipv6_addr_t  ndp_pending_ip  = IPV6_ZERO;
static mac_addr_t   ndp_pending_mac = MAC_ZERO;
static int          ndp_pending_done = 0;
static uint64_t     ndp_pending_started = 0;
static int          ndp_pending_tries = 0;

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

/* IPv6 handlers */
static void handle_ipv6(uint8_t* data, uint16_t len);
static void handle_icmpv6(ipv6_addr_t src, ipv6_addr_t dst, uint8_t* data, uint16_t len);
static void handle_ndp(uint8_t* data, uint16_t len, mac_addr_t src_mac);
static void ipv6_auto_config(void);
static int  eth_send_ipv6_raw(ipv6_addr_t dst, uint8_t next_hdr,
                               const void* payload, uint16_t payload_len);

/* TCP entry (defined in tcp.c) */
extern void tcp_handle(struct ip_hdr* ip, uint8_t* data, uint16_t len);
extern void tcp_handle6(ipv6_addr_t src, ipv6_addr_t dst, uint8_t* data, uint16_t len);
extern void tcp_tick(void);

/* HTTP server (defined in http_server.c) */
extern void http_server_tick(void);

/* Sandbox HTTP server (defined in sys/sandbox_server.c) */
extern void sandbox_server_tick(void);

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

    net_driver_send(buf, sizeof(buf));
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

/* ----- NDP (Neighbor Discovery Protocol for IPv6) ----- */
static mac_addr_t ndp_lookup(ipv6_addr_t ip) {
    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (ndp_cache[i].state != 0 && ipv6_eq(ndp_cache[i].ip, ip)) {
            return ndp_cache[i].mac;
        }
    }
    return MAC_ZERO;
}

static void ndp_cache_add(ipv6_addr_t ip, mac_addr_t mac, int state) {
    if (ipv6_is_zero(ip)) return;
    int empty = -1;
    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (ndp_cache[i].state == 0) { empty = i; break; }
        if (ipv6_eq(ndp_cache[i].ip, ip)) {
            ndp_cache[i].mac = mac;
            ndp_cache[i].state = state;
            ndp_cache[i].last_seen = timer_get_ms();
            return;
        }
    }
    if (empty >= 0) {
        ndp_cache[empty].ip = ip;
        ndp_cache[empty].mac = mac;
        ndp_cache[empty].state = state;
        ndp_cache[empty].last_seen = timer_get_ms();
    }
}

/* ----- IPv6 checksum (pseudo-header + data) ----- */
static uint16_t ipv6_l4_checksum(ipv6_addr_t src, ipv6_addr_t dst,
                                  uint8_t proto,
                                  const void* l4, uint16_t l4_len) {
    uint32_t sum = 0;
    /* Pseudo-header: src (16 bytes) + dst (16 bytes) + 4-byte length + next header */
    for (int i = 0; i < 16; i += 2)
        sum += ((uint16_t)src.bytes[i] << 8) | src.bytes[i+1];
    for (int i = 0; i < 16; i += 2)
        sum += ((uint16_t)dst.bytes[i] << 8) | dst.bytes[i+1];
    sum += proto;
    sum += l4_len;
    return inet_checksum(l4, l4_len, sum);
}

static void ndp_send_solicit(ipv6_addr_t target) {
    /* Build NS: ICMPv6 header (type=135, code=0, checksum) + reserved + target + SLL option */
    uint8_t buf[sizeof(struct eth_hdr) + sizeof(struct ipv6_hdr) +
                sizeof(struct icmpv6_hdr) + sizeof(struct icmpv6_ns) + 8];
    struct eth_hdr*    eth  = (struct eth_hdr*)&buf[0];
    struct ipv6_hdr*   ip6  = (struct ipv6_hdr*)&buf[sizeof(struct eth_hdr)];
    uint8_t*           icmp = &buf[sizeof(struct eth_hdr) + sizeof(struct ipv6_hdr)];
    struct icmpv6_hdr* ih   = (struct icmpv6_hdr*)icmp;
    struct icmpv6_ns*  ns   = (struct icmpv6_ns*)(icmp + sizeof(struct icmpv6_hdr));
    uint8_t*           opt  = icmp + sizeof(struct icmpv6_hdr) + sizeof(struct icmpv6_ns);

    /* NDP target address = our link-local for source LL option */
    ipv6_addr_t src_ll = ipv6_create_link_local(my_mac);

    /* Multicast MAC for solicited-node: ff02::1:ffXX:XXXX */
    mac_addr_t dst_mac;
    dst_mac.bytes[0] = 0x33;
    dst_mac.bytes[1] = 0x33;
    dst_mac.bytes[2] = target.bytes[13];
    dst_mac.bytes[3] = target.bytes[14];
    dst_mac.bytes[4] = target.bytes[15];
    dst_mac.bytes[5] = 0x00;

    eth->dst = dst_mac;
    eth->src = my_mac;
    eth->ethertype = htons16(ETH_TYPE_IPV6);

    /* IPv6 header */
    uint32_t ver = (6u << 28);
    ip6->ver_traffic_flow = htonl32(ver);
    uint16_t payload_len = sizeof(struct icmpv6_hdr) + sizeof(struct icmpv6_ns) + 8;
    ip6->payload_len = htons16(payload_len);
    ip6->next_header = IPV6_PROTO_ICMPV6;
    ip6->hop_limit = 255;
    ip6->src = src_ll;
    ip6->dst = target;

    /* ICMPv6 header */
    ih->type = ICMPV6_TYPE_NS;
    ih->code = 0;
    ih->checksum = 0;

    /* NS body */
    ns->reserved = 0;
    ns->target = target;

    /* Source LL addr option */
    opt[0] = NDP_OPT_SOURCE_LLADDR;
    opt[1] = 1;  /* 8 bytes */
    memcpy(&opt[2], my_mac.bytes, 6);

    /* ICMPv6 checksum over pseudo-header + ICMPv6 message */
    ih->checksum = htons16(ipv6_l4_checksum(src_ll, target,
                           IPV6_PROTO_ICMPV6, icmp, payload_len));

    net_driver_send(buf, sizeof(buf));
}

/* Resolve IPv6 address to MAC via NDP. Blocks up to timeout_ms. */
static mac_addr_t ndp_resolve(ipv6_addr_t ip, uint32_t timeout_ms) {
    mac_addr_t m = ndp_lookup(ip);
    if (m.bytes[0] || m.bytes[1] || m.bytes[2] ||
        m.bytes[3] || m.bytes[4] || m.bytes[5]) {
        return m;
    }
    if (ipv6_is_multicast(ip)) {
        /* Multicast MAC: 33:33:XX:XX:XX:XX */
        mac_addr_t mc;
        mc.bytes[0] = 0x33; mc.bytes[1] = 0x33;
        mc.bytes[2] = ip.bytes[12]; mc.bytes[3] = ip.bytes[13];
        mc.bytes[4] = ip.bytes[14]; mc.bytes[5] = ip.bytes[15];
        return mc;
    }
    ipv6_addr_t my_ll = ipv6_create_link_local(my_mac);
    if (ipv6_eq(ip, my_ll)) return my_mac;

    ndp_pending_ip = ip;
    ndp_pending_mac = MAC_ZERO;
    ndp_pending_done = 0;
    ndp_pending_started = timer_get_ms();
    ndp_pending_tries = 0;
    ndp_send_solicit(ip);

    uint64_t deadline = ndp_pending_started + timeout_ms;
    while (timer_get_ms() < deadline) {
        if (ndp_pending_done == 1) return ndp_pending_mac;
        if (timer_get_ms() > ndp_pending_started + (uint64_t)(ndp_pending_tries + 1) * 200) {
            ndp_pending_tries++;
            ndp_send_solicit(ip);
        }
        net_tick();
    }
    ndp_pending_done = -1;
    return MAC_ZERO;
}

/* Send an IPv6 packet over Ethernet. Returns bytes sent or 0 on failure. */
static int eth_send_ipv6_raw(ipv6_addr_t dst, uint8_t next_hdr,
                              const void* payload, uint16_t payload_len) {
    if (!net_link_up) return 0;

    mac_addr_t dst_mac = ndp_resolve(dst, 1000);
    int mac_any = dst_mac.bytes[0] | dst_mac.bytes[1] | dst_mac.bytes[2]
                | dst_mac.bytes[3] | dst_mac.bytes[4] | dst_mac.bytes[5];
    if (!mac_any && !ipv6_is_multicast(dst)) return 0;

    uint16_t total = sizeof(struct eth_hdr) + sizeof(struct ipv6_hdr) + payload_len;
    if (total > NET_MAX_PKT) return 0;

    static uint8_t buf[NET_MAX_PKT];
    struct eth_hdr*  eth = (struct eth_hdr*)&buf[0];
    struct ipv6_hdr* ip6 = (struct ipv6_hdr*)&buf[sizeof(struct eth_hdr)];
    uint8_t*         pl  = &buf[sizeof(struct eth_hdr) + sizeof(struct ipv6_hdr)];

    eth->dst = dst_mac;
    eth->src = my_mac;
    eth->ethertype = htons16(ETH_TYPE_IPV6);

    uint32_t ver = (6u << 28);
    ip6->ver_traffic_flow = htonl32(ver);
    ip6->payload_len = htons16(payload_len);
    ip6->next_header = next_hdr;
    ip6->hop_limit = 64;
    ip6->src = ipv6_create_link_local(my_mac);
    ip6->dst = dst;

    memcpy(pl, payload, payload_len);

    return net_driver_send(buf, total);
}

/* Public wrapper for tcp.c */
int eth_send_ipv6_pub(ipv6_addr_t dst, uint8_t next_hdr,
                       const void* payload, uint16_t payload_len) {
    return eth_send_ipv6_raw(dst, next_hdr, payload, payload_len);
}

/* ----- ICMPv6 handler ----- */
static uint16_t net_ping6_expect_id = 0;
static uint16_t net_ping6_expect_seq = 0;
static int      net_ping6_got_reply = 0;

static void handle_icmpv6(ipv6_addr_t src, ipv6_addr_t dst, uint8_t* data, uint16_t len) {
    if (len < 4) return;
    struct icmpv6_hdr* hdr = (struct icmpv6_hdr*)data;

    switch (hdr->type) {
    case ICMPV6_TYPE_ECHO_REQUEST: {
        /* Reply: swap src/dst, type -> 129 */
        uint8_t buf[sizeof(struct icmpv6_hdr) + 64];
        uint16_t paylen = len;
        if (paylen > sizeof(buf)) paylen = sizeof(buf);
        memcpy(buf, data, paylen);
        struct icmpv6_hdr* rh = (struct icmpv6_hdr*)buf;
        rh->type = ICMPV6_TYPE_ECHO_REPLY;
        rh->code = 0;
        rh->checksum = 0;
        rh->checksum = htons16(ipv6_l4_checksum(dst, src, IPV6_PROTO_ICMPV6, buf, paylen));
        eth_send_ipv6_raw(src, IPV6_PROTO_ICMPV6, buf, paylen);
        break;
    }
    case ICMPV6_TYPE_ECHO_REPLY: {
        struct icmpv6_echo* ec = (struct icmpv6_echo*)(data + 4);
        if (ntohs16(ec->id) == net_ping6_expect_id &&
            ntohs16(ec->seq) == net_ping6_expect_seq) {
            net_ping6_got_reply = 1;
        }
        break;
    }
    case ICMPV6_TYPE_NS: {
        /* Neighbor Solicitation: reply with Neighbor Advertisement */
        struct icmpv6_ns* ns = (struct icmpv6_ns*)(data + 4);
        if (len < sizeof(struct icmpv6_hdr) + 4 + 16) return;
        ipv6_addr_t target = ns->target;

        /* Cache the sender's MAC if SLL option present */
        if (len >= sizeof(struct icmpv6_hdr) + 4 + 16 + 8) {
            uint8_t* opt = data + sizeof(struct icmpv6_hdr) + 4 + 16;
            if (opt[0] == NDP_OPT_SOURCE_LLADDR && opt[1] == 1) {
                mac_addr_t sender_mac;
                memcpy(sender_mac.bytes, &opt[2], 6);
                ndp_cache_add(src, sender_mac, 2);
            }
        }

        /* Only reply if target is our address */
        ipv6_addr_t my_ll = ipv6_create_link_local(my_mac);
        if (ipv6_eq(target, my_ll) || ipv6_eq(target, my_ip6)) {
            /* Build NA */
            uint8_t buf[sizeof(struct icmpv6_hdr) + 4 + 16 + 8];
            memset(buf, 0, sizeof(buf));
            struct icmpv6_hdr* na_hdr = (struct icmpv6_hdr*)buf;
            struct icmpv6_na*  na = (struct icmpv6_na*)(buf + 4);
            na_hdr->type = ICMPV6_TYPE_NA;
            na_hdr->code = 0;
            na->flags = htonl32(0x60000000); /* R=0, S=1, O=1 */
            na->target = target;
            /* Target LL addr option */
            uint8_t* opt = buf + sizeof(struct icmpv6_hdr) + 4 + 16;
            opt[0] = NDP_OPT_TARGET_LLADDR;
            opt[1] = 1;
            memcpy(&opt[2], my_mac.bytes, 6);

            uint16_t paylen = sizeof(struct icmpv6_hdr) + 4 + 16 + 8;
            na_hdr->checksum = 0;
            na_hdr->checksum = htons16(ipv6_l4_checksum(my_ll, src,
                                     IPV6_PROTO_ICMPV6, buf, paylen));
            /* solicited NA unicast to sender */
            eth_send_ipv6_raw(src, IPV6_PROTO_ICMPV6, buf, paylen);
        }
        break;
    }
    case ICMPV6_TYPE_NA: {
        if (len < sizeof(struct icmpv6_hdr) + 4 + 16) return;
        struct icmpv6_na* na = (struct icmpv6_na*)(data + 4);
        mac_addr_t na_mac = MAC_ZERO;
        /* Extract target LL addr option */
        if (len >= sizeof(struct icmpv6_hdr) + 4 + 16 + 8) {
            uint8_t* opt = data + sizeof(struct icmpv6_hdr) + 4 + 16;
            if (opt[0] == NDP_OPT_TARGET_LLADDR && opt[1] == 1) {
                memcpy(na_mac.bytes, &opt[2], 6);
            }
        }
        int mac_any = na_mac.bytes[0] | na_mac.bytes[1] | na_mac.bytes[2]
                    | na_mac.bytes[3] | na_mac.bytes[4] | na_mac.bytes[5];
        if (mac_any) {
            ndp_cache_add(na->target, na_mac, 2);
        }
        if (ndp_pending_done == 0 && ipv6_eq(na->target, ndp_pending_ip)) {
            if (mac_any) ndp_pending_mac = na_mac;
            ndp_pending_done = 1;
        }
        break;
    }
    case ICMPV6_TYPE_RA: {
        /* Router Advertisement: auto-configure our IPv6 address */
        if (!ipv6_auto_done || !ipv6_valid) {
            /* Cache router's link-local as default gateway */
            if (ipv6_is_link_local(src) && ipv6_is_zero(my_ip6_gw)) {
                my_ip6_gw = src;
                pr_info("IPv6: default gateway set from RA\n");
            }
            handle_ndp(data, len, MAC_ZERO);
        }
        break;
    }
    }
}

/* ----- IPv6 auto-configuration ----- */
static void ipv6_auto_config(void) {
    if (ipv6_auto_done) return;
    ipv6_auto_done = 1;

    /* Our link-local address is derived from MAC */
    my_ip6 = ipv6_create_link_local(my_mac);
    ipv6_valid = 1;
    pr_info("IPv6: link-local address configured\n");

    /* Send Router Solicitation to ff02::2 */
    uint8_t buf[sizeof(struct icmpv6_hdr) + 4];
    memset(buf, 0, sizeof(buf));
    struct icmpv6_hdr* rs = (struct icmpv6_hdr*)buf;
    rs->type = ICMPV6_TYPE_RS;
    rs->code = 0;

    ipv6_addr_t all_routers = IPV6_ALL_ROUTERS;
    uint16_t paylen = sizeof(struct icmpv6_hdr) + 4;
    rs->checksum = 0;
    rs->checksum = htons16(ipv6_l4_checksum(my_ip6, all_routers,
                           IPV6_PROTO_ICMPV6, buf, paylen));

    eth_send_ipv6_raw(all_routers, IPV6_PROTO_ICMPV6, buf, paylen);
    pr_info("IPv6: sent Router Solicitation\n");
}

/* Handle NDP messages (RA with prefix info for SLAAC) */
static void handle_ndp(uint8_t* data, uint16_t len, mac_addr_t src_mac) {
    (void)src_mac;
    if (len < sizeof(struct icmpv6_hdr) + sizeof(struct icmpv6_ra)) return;
    struct icmpv6_hdr* hdr = (struct icmpv6_hdr*)data;
    if (hdr->type != ICMPV6_TYPE_RA) return;

    /* The router's link-local address is my_ip6_gw (set by caller in handle_icmpv6).
     * Walk options to find Prefix Information for SLAAC. */
    uint8_t* opt = data + sizeof(struct icmpv6_hdr) + sizeof(struct icmpv6_ra);
    uint8_t* end = data + len;
    while (opt + 2 <= end) {
        uint8_t opt_type = opt[0];
        uint8_t opt_len  = opt[1];
        if (opt_len == 0 || opt + opt_len * 8 > end) break;
        if (opt_type == NDP_OPT_PREFIX_INFO && opt_len == 4) {
            struct ndp_prefix_info* pinfo = (struct ndp_prefix_info*)opt;
            if (pinfo->flags & 0x40) {  /* A bit: autonomous address-configuration */
                /* Form global address: prefix + our interface ID */
                ipv6_addr_t new_addr = IPV6_ZERO;
                int prefix_bytes = (pinfo->prefix_len + 7) / 8;
                if (prefix_bytes > 16) prefix_bytes = 16;
                memcpy(new_addr.bytes, pinfo->prefix.bytes, prefix_bytes);
                /* Fill remaining bytes with our EUI-64 (interface ID) */
                ipv6_addr_t ll = ipv6_create_link_local(my_mac);
                for (int i = prefix_bytes; i < 16; i++) {
                    new_addr.bytes[i] = ll.bytes[i];
                }
                my_ip6 = new_addr;
                ipv6_valid = 1;

                /* Cache the router's link-local as gateway */
                /* (gateway was already set in handle_icmpv6 RA case) */
                pr_info("IPv6: SLAAC - address %02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                        ":%02x%02x:%02x%02x:%02x%02x:%02x%02x\n",
                        new_addr.bytes[0], new_addr.bytes[1],
                        new_addr.bytes[2], new_addr.bytes[3],
                        new_addr.bytes[4], new_addr.bytes[5],
                        new_addr.bytes[6], new_addr.bytes[7],
                        new_addr.bytes[8], new_addr.bytes[9],
                        new_addr.bytes[10], new_addr.bytes[11],
                        new_addr.bytes[12], new_addr.bytes[13],
                        new_addr.bytes[14], new_addr.bytes[15]);
            }
        }
        opt += opt_len * 8;
    }
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

    /* Firewall check (outgoing) */
    enum fw_action fw_act = fw_check(ip, 1);
    if (fw_act == FW_DROP) {
        return 0;
    }

    /* ip_checksum returns a value in HOST byte order (the one's-complement
     * of the sum). The wire format expects network (big-endian) byte
     * order, so we must htons16 it before storing into the struct field
     * (which on x86_64 is little-endian). */
    ip->checksum = htons16(ip_checksum(ip, sizeof(struct ip_hdr)));

    memcpy(pl, payload, payload_len);

    return net_driver_send(buf, total);
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

/* Send a UDP datagram over IPv6. Returns bytes sent or 0 on failure. */
int udp_send6(ipv6_addr_t dst_ip, uint16_t src_port, uint16_t dst_port,
              const void* payload, uint16_t payload_len) {
    uint8_t buf[sizeof(struct udp_hdr) + 1500];
    struct udp_hdr* u = (struct udp_hdr*)buf;
    u->src_port = htons16(src_port);
    u->dst_port = htons16(dst_port);
    u->length   = htons16(sizeof(struct udp_hdr) + payload_len);
    /* IPv6 mandates a pseudo-header checksum for UDP */
    ipv6_addr_t src_ll = ipv6_create_link_local(my_mac);
    memcpy(&buf[sizeof(struct udp_hdr)], payload, payload_len);
    uint16_t total = sizeof(struct udp_hdr) + payload_len;
    u->checksum = 0;
    u->checksum = htons16(ipv6_l4_checksum(src_ll, dst_ip, IPV6_PROTO_UDP, buf, total));

    udp_expect_src_port = dst_port;
    udp_expect_dst_port = src_port;
    udp_pending_len = 0;

    if (!eth_send_ipv6_raw(dst_ip, IPV6_PROTO_UDP, buf, total)) {
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
    net_driver_send(buf, sizeof(buf));
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
        net_driver_send(buf, sizeof(buf));
    }
}

/* ----- IPv6 packet dispatch ----- */
static void handle_ipv6(uint8_t* data, uint16_t len) {
    if (len < sizeof(struct ipv6_hdr)) return;
    struct ipv6_hdr* ip6 = (struct ipv6_hdr*)data;

    uint32_t ver_flow = ntohl32(ip6->ver_traffic_flow);
    uint8_t version = (ver_flow >> 28) & 0xF;
    if (version != 6) return;

    uint16_t payload_len = ntohs16(ip6->payload_len);
    uint8_t  next_header = ip6->next_header;

    /* Verify destination matches us */
    ipv6_addr_t my_ll = ipv6_create_link_local(my_mac);
    int for_us = ipv6_eq(ip6->dst, my_ip6) ||
                 ipv6_eq(ip6->dst, my_ll) ||
                 ipv6_eq(ip6->dst, IPV6_LOOPBACK) ||
                 ipv6_is_multicast(ip6->dst);
    if (!for_us) return;

    uint8_t* l4 = &data[sizeof(struct ipv6_hdr)];
    uint16_t l4_len = (payload_len <= len - sizeof(struct ipv6_hdr)) ?
                       payload_len : len - sizeof(struct ipv6_hdr);

    switch (next_header) {
        case IPV6_PROTO_ICMPV6:
            handle_icmpv6(ip6->src, ip6->dst, l4, l4_len);
            break;
        case IPV6_PROTO_TCP:
            tcp_handle6(ip6->src, ip6->dst, l4, l4_len);
            break;
        case IPV6_PROTO_UDP:
            /* TODO: dispatch to handle_udp6 when ready */
            break;
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

    /* Firewall check (incoming) */
    enum fw_action fw_act = fw_check(ip, 0);
    if (fw_act == FW_DROP) {
        return;
    }
    if (fw_act == FW_REJECT) {
        /* Send ICMP port unreachable (type 3, code 3) for TCP/UDP */
        if (ip->proto == IP_PROTO_TCP || ip->proto == IP_PROTO_UDP) {
            uint8_t icmp_buf[sizeof(struct icmp_hdr) + sizeof(struct ip_hdr) + 8];
            memset(icmp_buf, 0, sizeof(icmp_buf));
            struct icmp_hdr* icmph = (struct icmp_hdr*)icmp_buf;
            icmph->type = 3;   /* Destination Unreachable */
            icmph->code = 3;   /* Port Unreachable */
            icmph->checksum = 0;
            /* Copy the original IP header + first 8 bytes of payload */
            uint16_t orig_ihl_bytes = ip_hdr_len;
            memcpy(&icmp_buf[sizeof(struct icmp_hdr)], ip, orig_ihl_bytes > 20 ? 20 : orig_ihl_bytes);
            uint16_t copy_len = (l4_len > 8) ? 8 : l4_len;
            memcpy(&icmp_buf[sizeof(struct icmp_hdr) + orig_ihl_bytes], l4, copy_len);
            icmph->checksum = htons16(ip_checksum(icmp_buf, sizeof(icmp_buf)));
            eth_send_ipv4(ip->src, IP_PROTO_ICMP, icmp_buf, sizeof(icmp_buf));
        }
        return;
    }

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
        case ETH_TYPE_IPV6:
            handle_ipv6(payload, payload_len);
            break;
        case ETH_TYPE_WLAN_MGMT:
            wifi_handle_frame(payload, payload_len);
            break;
        case ETH_TYPE_EAPOL:
            wifi_handle_eapol_frame(payload, payload_len);
            break;
        /* ignore other protocols */
    }
}

/* ----- public API ----- */
void net_init(void) {
    pr_info("net: initializing network stack\n");
    /* Probe each NIC driver in priority order via the vtable.
     * First one that initializes successfully becomes active. */
    for (int i = 0; i < nic_driver_count && nic_driver_table[i]; i++) {
        const struct nic_ops *ops = nic_driver_table[i];
        if (ops->init && ops->init()) {
            active_nic_ops = ops;
            net_iface_name = (char*)ops->name;
            my_mac = ops->get_mac();
            pr_info("net: using %s driver\n", ops->name);
            goto nic_found;
        }
    }
    pr_warn("net: no NIC - networking disabled\n");
    return;
nic_found:
    net_initialized = 1;
    net_link_up = 1;   /* link is up from driver perspective; IP not yet */
    fw_init();
    dhcp_start();
    /* IPv6 auto-configuration (link-local + SLAAC) */
    ipv6_auto_config();
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
        int n = net_driver_recv(buf, sizeof(buf));
        if (n <= 0) break;
        rx_count++;
        /* Check for ICMP Echo Reply (for our own pings) or ICMPv6 Echo Reply. */
        if (n >= (int)sizeof(struct eth_hdr)) {
            struct eth_hdr* eth = (struct eth_hdr*)buf;
            if (ntohs16(eth->ethertype) == ETH_TYPE_IPV4 &&
                n >= (int)(sizeof(struct eth_hdr) + sizeof(struct ip_hdr))) {
                struct ip_hdr* ip = (struct ip_hdr*)&buf[sizeof(struct eth_hdr)];
                if (ip->proto == IP_PROTO_ICMP) {
                    uint8_t ihl = ip->ver_ihl & 0x0F;
                    if (sizeof(struct eth_hdr) + ihl*4 + sizeof(struct icmp_hdr) <= (size_t)n) {
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
            /* Check for ICMPv6 Echo Reply */
            if (ntohs16(eth->ethertype) == ETH_TYPE_IPV6 &&
                n >= (int)(sizeof(struct eth_hdr) + sizeof(struct ipv6_hdr) + 8)) {
                struct ipv6_hdr* ip6 = (struct ipv6_hdr*)&buf[sizeof(struct eth_hdr)];
                if (ip6->next_header == IPV6_PROTO_ICMPV6) {
                    struct icmpv6_hdr* icmp6 = (struct icmpv6_hdr*)
                        &buf[sizeof(struct eth_hdr) + sizeof(struct ipv6_hdr)];
                    if (icmp6->type == ICMPV6_TYPE_ECHO_REPLY &&
                        n >= (int)(sizeof(struct eth_hdr) + sizeof(struct ipv6_hdr) + 8)) {
                        struct icmpv6_echo* ec = (struct icmpv6_echo*)(icmp6 + 1);
                        if (ntohs16(ec->id) == net_ping6_expect_id &&
                            ntohs16(ec->seq) == net_ping6_expect_seq) {
                            net_ping6_got_reply = 1;
                            continue;
                        }
                    }
                }
            }
        }
        /* Otherwise dispatch to the right protocol handler. */
        handle_ethernet(buf, (uint16_t)n);
    }
    /* Post-batch flush for drivers that need it (e.g. RTL8139 CAPR) */
    if (active_nic_ops && active_nic_ops->flush)
        active_nic_ops->flush();

    /* Advance DHCP and TCP state machines. */
    dhcp_tick();
    tcp_tick();
    http_server_tick();
    sandbox_server_tick();

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
const char*  net_get_iface_name(void){ return net_iface_name; }

/* Expose eth_send_ipv4 and udp_send to TCP and HTTP layers */
int eth_send_ipv4_pub(ipv4_addr_t dst_ip, uint8_t proto,
                       const void* payload, uint16_t payload_len) {
    return eth_send_ipv4(dst_ip, proto, payload, payload_len);
}

/* Setter functions for net_config module (static IP override) */
void net_set_mask(ipv4_addr_t mask) { my_mask = mask; }
void net_set_gw(ipv4_addr_t gw)     { my_gw = gw; }
void net_set_dns(ipv4_addr_t dns)   { my_dns = dns; }

/* ----- IPv6 public API ----- */
int net_ipv6_is_valid(void) { return ipv6_valid; }
ipv6_addr_t net_get_ipv6(void) { return my_ip6; }
ipv6_addr_t net_get_ipv6_gw(void) { return my_ip6_gw; }

int net_ping6(ipv6_addr_t target, uint16_t seq, uint32_t timeout_ms) {
    if (!net_link_up || !ipv6_valid) return 0;

    /* Build ICMPv6 Echo Request */
    uint8_t buf[sizeof(struct icmpv6_hdr) + sizeof(struct icmpv6_echo) + 32];
    memset(buf, 0, sizeof(buf));
    struct icmpv6_hdr*  icmp6 = (struct icmpv6_hdr*)buf;
    struct icmpv6_echo* ec    = (struct icmpv6_echo*)(buf + 4);
    icmp6->type = ICMPV6_TYPE_ECHO_REQUEST;
    icmp6->code = 0;
    ec->id   = htons16(0xBEEF);
    ec->seq  = htons16(seq);

    uint16_t paylen = sizeof(buf);
    icmp6->checksum = 0;
    icmp6->checksum = htons16(ipv6_l4_checksum(my_ip6, target,
                             IPV6_PROTO_ICMPV6, buf, paylen));

    net_ping6_expect_id = ntohs16(ec->id);
    net_ping6_expect_seq = ntohs16(ec->seq);
    net_ping6_got_reply = 0;

    if (!eth_send_ipv6_raw(target, IPV6_PROTO_ICMPV6, buf, paylen))
        return 0;

    uint64_t deadline = timer_get_ms() + timeout_ms;
    while (timer_get_ms() < deadline) {
        if (net_ping6_got_reply) return 1;
        net_tick();
    }
    return 0;
}

/* ----- DNS dual-stack ----- */
int net_resolve_dual(const char* hostname, ipv4_addr_t* out4, ipv6_addr_t* out6) {
    int got4 = 0, got6 = 0;

    /* Try to parse as IPv6 literal (contains ':') */
    int has_colon = 0;
    for (const char* p = hostname; *p; p++) {
        if (*p == ':') { has_colon = 1; break; }
    }
    if (has_colon) {
        /* Parse IPv6 hex address */
        ipv6_addr_t addr = IPV6_ZERO;
        int group = 0, shift = 0;
        int skip_group = -1; /* where :: expands from */
        const char* p = hostname;
        if (hostname[0] == '[') p++;  /* allow [addr] bracket form */
        while (*p && *p != ']' && group < 8) {
            if (*p == ':') {
                if (p[1] == ':') {
                    skip_group = group;
                    p += 2;
                    if (*p == ':') { p++; continue; }
                    continue;
                }
                group++;
                shift = 0;
                p++;
            } else {
                int val = -1;
                if (*p >= '0' && *p <= '9') val = *p - '0';
                else if (*p >= 'a' && *p <= 'f') val = *p - 'a' + 10;
                else if (*p >= 'A' && *p <= 'F') val = *p - 'A' + 10;
                if (val < 0) break;
                addr.bytes[group*2]   = (addr.bytes[group*2] << 4) | val;
                shift++;
                if (shift > 4) break;
                p++;
            }
        }
        if (shift > 0 || group > 0 || skip_group >= 0) {
            /* Compact the address if :: was used */
            if (skip_group >= 0) {
                int end = group;
                int skip_count = 7 - end;
                for (int i = end; i >= 0 && i >= skip_group; i--) {
                    addr.bytes[(i + skip_count)*2]   = addr.bytes[i*2];
                    addr.bytes[(i + skip_count)*2+1] = addr.bytes[i*2+1];
                }
                for (int i = skip_group; i < skip_group + skip_count && i < 8; i++) {
                    addr.bytes[i*2] = 0;
                    addr.bytes[i*2+1] = 0;
                }
            }
            if (out6) *out6 = addr;
            return 1;
        }
    }

    /* Try AAAA first */
    if (out6) {
        /* Build AAAA query */
        static uint8_t qbuf[512];
        struct dns_hdr* hdr = (struct dns_hdr*)qbuf;
        memset(hdr, 0, sizeof(*hdr));
        hdr->id = htons16(0x4243);
        hdr->flags = htons16(0x0100);
        hdr->qdcount = htons16(1);
        uint8_t* p = &qbuf[sizeof(struct dns_hdr)];
        int namelen = dns_encode_name(p, sizeof(qbuf) - sizeof(struct dns_hdr) - 4, hostname);
        if (namelen >= 0) {
            p += namelen;
            *p++ = 0; *p++ = 28;  /* qtype = AAAA (28) */
            *p++ = 0; *p++ = 1;   /* qclass = IN */
            uint16_t qlen = (uint16_t)(p - qbuf);
            if (udp_send(my_dns, 12346, DNS_PORT, qbuf, qlen)) {
                static uint8_t rbuf[1500];
                int rlen = udp_recv_wait(rbuf, sizeof(rbuf), 2000);
                if (rlen >= (int)sizeof(struct dns_hdr)) {
                    struct dns_hdr* rhdr = (struct dns_hdr*)rbuf;
                    if (rhdr->id == htons16(0x4243)) {
                        uint16_t ancount = ntohs16(rhdr->ancount);
                        uint8_t* q = &rbuf[sizeof(struct dns_hdr)];
                        char tmp[DNS_MAX_NAME];
                        int consumed = dns_decode_name(rbuf, rlen, q, tmp, sizeof(tmp));
                        if (consumed >= 0) {
                            q += consumed + 4;
                            for (int i = 0; i < (int)ancount; i++) {
                                consumed = dns_decode_name(rbuf, rlen, q, tmp, sizeof(tmp));
                                if (consumed < 0) break;
                                q += consumed;
                                if (q + 10 > rbuf + rlen) break;
                                uint16_t qtype = (q[0] << 8) | q[1];
                                uint16_t rdlen = (q[8] << 8) | q[9];
                                q += 10;
                                if (qtype == 28 && rdlen == 16) {
                                    memcpy(out6->bytes, q, 16);
                                    got6 = 1;
                                }
                                q += rdlen;
                            }
                        }
                    }
                }
            }
        }
    }

    /* Then try A record */
    if (out4) {
        got4 = net_resolve(hostname, out4);
    }

    return got4 || got6;
}
