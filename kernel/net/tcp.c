/*
 * Lestra OS - TCP state machine with multi-connection support
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Supports up to TCP_MAX_CONNS concurrent connections (client + server).
 * conn[0] is the default client connection used by the legacy API
 * (tcp_connect/tcp_send/tcp_recv_wait/tcp_close) for backward compat
 * with http.c, tls.c, ai.c.
 */

#include <lestra/types.h>
#include <lestra/net.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <string.h>

struct ip_hdr_pub {
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

struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint16_t data_off_flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urg_ptr;
} __packed;

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

struct tcp_pseudo {
    ipv4_addr_t src;
    ipv4_addr_t dst;
    uint8_t  zero;
    uint8_t  proto;
    uint16_t tcp_len;
} __packed;

static struct tcp_conn conns[TCP_MAX_CONNS];
static uint16_t next_local_port = 0x4000;

#define TCP_RETRANSMIT_TIMEOUT_MS  2000
#define TCP_MAX_RETRANSMITS        5

extern int eth_send_ipv4_pub(ipv4_addr_t dst_ip, uint8_t proto,
                              const void* payload, uint16_t payload_len);
extern int eth_send_ipv6_pub(ipv6_addr_t dst, uint8_t next_hdr,
                              const void* payload, uint16_t payload_len);
extern ipv4_addr_t my_ip;

struct tcp_conn* tcp_get_conn(int idx) {
    if (idx < 0 || idx >= TCP_MAX_CONNS) return NULL;
    return &conns[idx];
}

static uint16_t tcp_checksum(ipv4_addr_t src, ipv4_addr_t dst,
                              const void* seg, uint16_t seg_len) {
    struct tcp_pseudo ph;
    ph.src = src;
    ph.dst = dst;
    ph.zero = 0;
    ph.proto = 6;
    ph.tcp_len = htons16(seg_len);

    uint32_t sum = 0;
    const uint8_t* p = (const uint8_t*)&ph;
    for (int i = 0; i < 12; i += 2) {
        sum += ((uint16_t)p[i] << 8) | p[i+1];
    }
    p = (const uint8_t*)seg;
    for (uint16_t i = 0; i + 1 < seg_len; i += 2) {
        sum += ((uint16_t)p[i] << 8) | p[i+1];
    }
    if (seg_len & 1) {
        sum += (uint16_t)p[seg_len-1] << 8;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static uint16_t tcp_checksum6(ipv6_addr_t src, ipv6_addr_t dst,
                               const void* seg, uint16_t seg_len) {
    uint32_t sum = 0;
    for (int i = 0; i < 16; i += 2)
        sum += ((uint16_t)src.bytes[i] << 8) | src.bytes[i+1];
    for (int i = 0; i < 16; i += 2)
        sum += ((uint16_t)dst.bytes[i] << 8) | dst.bytes[i+1];
    sum += 6;  /* TCP protocol number */
    sum += seg_len;
    const uint8_t* p = (const uint8_t*)seg;
    for (uint16_t i = 0; i + 1 < seg_len; i += 2) {
        sum += ((uint16_t)p[i] << 8) | p[i+1];
    }
    if (seg_len & 1) {
        sum += (uint16_t)p[seg_len-1] << 8;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static int tcp_send_seg(struct tcp_conn* c, uint8_t flags,
                         const void* payload, uint16_t payload_len) {
    uint8_t buf[sizeof(struct tcp_hdr) + 1500];
    struct tcp_hdr* h = (struct tcp_hdr*)buf;
    h->src_port = htons16(c->local_port);
    h->dst_port = htons16(c->peer_port);
    h->seq      = htonl32(c->tx_seq);
    h->ack      = htonl32(c->rx_seq);
    h->data_off_flags = htons16((5 << 12) | flags);
    h->window   = htons16(8192);
    h->checksum = 0;
    h->urg_ptr  = 0;
    if (payload_len && payload) {
        memcpy(&buf[sizeof(struct tcp_hdr)], payload, payload_len);
    }
    uint16_t seg_len = sizeof(struct tcp_hdr) + payload_len;

    if (c->is_ipv6) {
        mac_addr_t mac = net_get_mac();
        ipv6_addr_t src_ll = ipv6_create_link_local(mac);
        h->checksum = htons16(tcp_checksum6(src_ll, c->peer_ip6, buf, seg_len));
    } else {
        h->checksum = htons16(tcp_checksum(my_ip, c->peer_ip, buf, seg_len));
    }

    memcpy(c->last_seg, buf, seg_len);
    c->last_seg_len = seg_len;
    c->last_seg_time = timer_get_ms();
    c->retransmit_count = 0;

    int r;
    if (c->is_ipv6) {
        r = eth_send_ipv6_pub(c->peer_ip6, 6, buf, seg_len);
    } else {
        r = eth_send_ipv4_pub(c->peer_ip, 6, buf, seg_len);
    }
    c->tx_seq += payload_len;
    if (flags & (TCP_SYN | TCP_FIN)) c->tx_seq += 1;
    return r;
}

static uint16_t alloc_local_port(void) {
    uint16_t p = next_local_port++;
    if (next_local_port < 0x4000) next_local_port = 0x4000;
    return p;
}

/* ----- legacy client API (conn[0]) ----- */

int tcp_connect(ipv4_addr_t dst_ip, uint16_t dst_port, uint32_t timeout_ms) {
    struct tcp_conn* c = &conns[0];
    if (c->state != TCP_CLOSED) return 0;
    if (!net_is_up()) return 0;

    c->in_use = 1;
    c->peer_ip = dst_ip;
    c->peer_port = dst_port;
    c->local_port = alloc_local_port();
    c->tx_seq = 0x12345u ^ (uint32_t)timer_get_ms();
    c->rx_seq = 0;
    c->rx_len = 0;
    c->rx_closed = 0;
    c->tcp_connected = 0;
    c->tcp_connect_failed = 0;
    c->is_server = 0;
    c->pending_accept = 0;
    c->accepted = NULL;
    c->last_seg_len = 0;
    c->is_ipv6 = 0;
    c->peer_ip6 = IPV6_ZERO;

    if (!tcp_send_seg(c, TCP_SYN, NULL, 0)) return 0;
    c->state = TCP_SYN_SENT;

    uint64_t deadline = timer_get_ms() + timeout_ms;
    while (timer_get_ms() < deadline) {
        if (c->tcp_connected) return 1;
        if (c->tcp_connect_failed) return 0;
        net_tick();
    }
    pr_warn("tcp_connect: timed out waiting for SYN-ACK\n");
    c->state = TCP_CLOSED;
    return 0;
}

int tcp_connect6(ipv6_addr_t dst_ip, uint16_t dst_port, uint32_t timeout_ms) {
    struct tcp_conn* c = &conns[0];
    if (c->state != TCP_CLOSED) return 0;
    if (!net_ipv6_is_valid()) return 0;

    c->in_use = 1;
    c->peer_ip = IP_ZERO;
    c->peer_ip6 = dst_ip;
    c->is_ipv6 = 1;
    c->peer_port = dst_port;
    c->local_port = alloc_local_port();
    c->tx_seq = 0x12345u ^ (uint32_t)timer_get_ms();
    c->rx_seq = 0;
    c->rx_len = 0;
    c->rx_closed = 0;
    c->tcp_connected = 0;
    c->tcp_connect_failed = 0;
    c->is_server = 0;
    c->pending_accept = 0;
    c->accepted = NULL;
    c->last_seg_len = 0;

    if (!tcp_send_seg(c, TCP_SYN, NULL, 0)) return 0;
    c->state = TCP_SYN_SENT;

    uint64_t deadline = timer_get_ms() + timeout_ms;
    while (timer_get_ms() < deadline) {
        if (c->tcp_connected) return 1;
        if (c->tcp_connect_failed) return 0;
        net_tick();
    }
    pr_warn("tcp_connect6: timed out waiting for SYN-ACK\n");
    c->state = TCP_CLOSED;
    return 0;
}

int tcp_send(const void* data, uint16_t len) {
    return tcp_send_conn(&conns[0], data, len);
}

int tcp_recv_wait(uint8_t* buf, uint16_t bufsz, uint32_t timeout_ms) {
    return tcp_recv_conn(&conns[0], buf, bufsz, timeout_ms);
}

void tcp_close(void) {
    tcp_close_conn(&conns[0]);
}

int tcp_is_closed(void) {
    return conns[0].state == TCP_CLOSED;
}

/* ----- connection-specific API ----- */

int tcp_send_conn(struct tcp_conn* c, const void* data, uint16_t len) {
    if (c->state != TCP_ESTABLISHED) return 0;
    if (len > 1400) len = 1400;
    return tcp_send_seg(c, TCP_PSH | TCP_ACK, data, len);
}

int tcp_recv_conn(struct tcp_conn* c, uint8_t* buf, uint16_t bufsz, uint32_t timeout_ms) {
    uint64_t deadline = timer_get_ms() + timeout_ms;
    uint16_t copied = 0;
    while (timer_get_ms() < deadline) {
        if (c->rx_len > 0) {
            uint16_t n = c->rx_len;
            if (n > bufsz - copied) n = bufsz - copied;
            memcpy(&buf[copied], c->rx_buf, n);
            memmove(c->rx_buf, &c->rx_buf[n], c->rx_len - n);
            c->rx_len -= n;
            copied += n;
        }
        if (c->rx_closed) return copied;
        if (copied >= bufsz) return copied;
        net_tick();
    }
    return copied;
}

void tcp_close_conn(struct tcp_conn* c) {
    if (c->state == TCP_ESTABLISHED) {
        tcp_send_seg(c, TCP_FIN | TCP_ACK, NULL, 0);
        c->state = TCP_FIN_WAIT_1;
    } else if (c->state == TCP_CLOSE_WAIT) {
        tcp_send_seg(c, TCP_FIN | TCP_ACK, NULL, 0);
        c->state = TCP_LAST_ACK;
    } else {
        c->state = TCP_CLOSED;
    }
}

/* ----- server API ----- */

int tcp_listen(uint16_t port, int backlog) {
    (void)backlog;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!conns[i].in_use || conns[i].state == TCP_CLOSED) {
            memset(&conns[i], 0, sizeof(conns[i]));
            conns[i].in_use = 1;
            conns[i].state = TCP_LISTEN;
            conns[i].is_server = 1;
            conns[i].local_port = port;
            pr_info("tcp: listening on port %u (slot %d)\n", (unsigned)port, i);
            return i;
        }
    }
    pr_warn("tcp_listen: no free slots\n");
    return -1;
}

int tcp_accept(int listen_idx, struct tcp_conn** out_conn) {
    if (listen_idx < 0 || listen_idx >= TCP_MAX_CONNS) return -1;
    struct tcp_conn* lc = &conns[listen_idx];
    if (!lc->is_server) return -1;

    uint64_t deadline = timer_get_ms() + 30000;
    while (timer_get_ms() < deadline) {
        if (lc->pending_accept) {
            lc->pending_accept = 0;
            *out_conn = lc->accepted;
            lc->accepted = NULL;
            return 0;
        }
        net_tick();
    }
    return -1;
}

/* ----- packet demux (called from net.c) ----- */

static struct tcp_conn* find_conn(ipv4_addr_t src_ip, uint16_t src_port, uint16_t dst_port) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!conns[i].in_use || conns[i].is_server) continue;
        if (conns[i].is_ipv6) continue;  /* IPv6 lookups use find_conn6 */
        if (ipv4_eq(conns[i].peer_ip, src_ip) &&
            conns[i].peer_port == src_port &&
            conns[i].local_port == dst_port) {
            return &conns[i];
        }
    }
    return NULL;
}

static struct tcp_conn* find_conn6(ipv6_addr_t src_ip, uint16_t src_port, uint16_t dst_port) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!conns[i].in_use || conns[i].is_server) continue;
        if (!conns[i].is_ipv6) continue;  /* IPv4 lookups use find_conn */
        if (ipv6_eq(conns[i].peer_ip6, src_ip) &&
            conns[i].peer_port == src_port &&
            conns[i].local_port == dst_port) {
            return &conns[i];
        }
    }
    return NULL;
}

static struct tcp_conn* find_listener(uint16_t port) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!conns[i].in_use || !conns[i].is_server) continue;
        if (conns[i].local_port == port) return &conns[i];
    }
    return NULL;
}

static int alloc_conn_slot(void) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!conns[i].in_use || conns[i].state == TCP_CLOSED) return i;
    }
    return -1;
}

void tcp_handle(struct ip_hdr_pub* ip_pub, uint8_t* data, uint16_t len) {
    if (len < sizeof(struct tcp_hdr)) return;
    struct ip_hdr_pub ip = *ip_pub;
    struct tcp_hdr* h = (struct tcp_hdr*)data;
    uint8_t  data_off = (ntohs16(h->data_off_flags) >> 12) & 0xF;
    uint16_t hdr_len = data_off * 4;
    if (hdr_len < 20 || hdr_len > len) return;
    uint16_t flags = ntohs16(h->data_off_flags) & 0x1FF;
    uint8_t* payload = &data[hdr_len];
    uint16_t payload_len = len - hdr_len;

    uint16_t src_port = ntohs16(h->src_port);
    uint16_t dst_port = ntohs16(h->dst_port);
    uint32_t seg_seq = ntohl32(h->seq);

    if (flags & TCP_RST) {
        struct tcp_conn* c = find_conn(ip.src, src_port, dst_port);
        if (c) {
            pr_warn("tcp: RST on conn (slot %d), aborting\n",
                    (int)(c - conns));
            c->state = TCP_CLOSED;
            c->tcp_connect_failed = 1;
            c->last_seg_len = 0;
        }
        return;
    }

    if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
        struct tcp_conn* listener = find_listener(dst_port);
        if (listener) {
            int slot = alloc_conn_slot();
            if (slot < 0) {
                pr_warn("tcp: no free slots for new connection\n");
                return;
            }
            struct tcp_conn* nc = &conns[slot];
            memset(nc, 0, sizeof(*nc));
            nc->in_use = 1;
            nc->peer_ip = ip.src;
            nc->peer_port = src_port;
            nc->local_port = dst_port;
            nc->tx_seq = 0x56789u ^ (uint32_t)timer_get_ms();
            nc->rx_seq = seg_seq + 1;
            tcp_send_seg(nc, TCP_SYN | TCP_ACK, NULL, 0);
            nc->state = TCP_SYN_RECEIVED;

            listener->pending_accept = 1;
            listener->accepted = nc;

            pr_info("tcp: SYN from %u.%u.%u.%u:%u -> port %u (slot %d)\n",
                    ip.src.bytes[0], ip.src.bytes[1],
                    ip.src.bytes[2], ip.src.bytes[3],
                    (unsigned)src_port, (unsigned)dst_port, slot);
            return;
        }
        return;
    }

    struct tcp_conn* c = find_conn(ip.src, src_port, dst_port);
    if (!c) return;

    if (c->state == TCP_SYN_SENT && (flags & TCP_SYN) && (flags & TCP_ACK)) {
        c->rx_seq = seg_seq + 1;
        c->tx_seq = ntohl32(h->ack);
        tcp_send_seg(c, TCP_ACK, NULL, 0);
        c->state = TCP_ESTABLISHED;
        c->tcp_connected = 1;
        c->last_seg_len = 0;
        return;
    }

    if (c->state == TCP_SYN_RECEIVED && (flags & TCP_ACK)) {
        c->state = TCP_ESTABLISHED;
        c->last_seg_len = 0;
        return;
    }

    if (c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 ||
        c->state == TCP_FIN_WAIT_2 || c->state == TCP_CLOSE_WAIT) {
        if (seg_seq != c->rx_seq) {
            if (flags & TCP_ACK) {
                tcp_send_seg(c, TCP_ACK, NULL, 0);
            }
            if (!(flags & TCP_FIN)) return;
        }

        if (payload_len > 0 && (flags & (TCP_PSH | TCP_ACK))) {
            if (c->rx_len + payload_len <= (uint16_t)sizeof(c->rx_buf)) {
                memcpy(&c->rx_buf[c->rx_len], payload, payload_len);
                c->rx_len += payload_len;
            }
            c->rx_seq += payload_len;
            tcp_send_seg(c, TCP_ACK, NULL, 0);
        }

        if (flags & TCP_FIN) {
            c->rx_seq += 1;
            tcp_send_seg(c, TCP_ACK, NULL, 0);
            if (c->state == TCP_ESTABLISHED) {
                c->state = TCP_CLOSE_WAIT;
                c->rx_closed = 1;
                c->last_seg_len = 0;
                tcp_send_seg(c, TCP_FIN | TCP_ACK, NULL, 0);
                c->state = TCP_LAST_ACK;
            } else if (c->state == TCP_FIN_WAIT_1) {
                c->state = TCP_CLOSING;
                c->state = TCP_CLOSED;
            } else if (c->state == TCP_FIN_WAIT_2) {
                c->state = TCP_CLOSED;
            }
        }
    }

    if (c->state == TCP_LAST_ACK && (flags & TCP_ACK)) {
        c->state = TCP_CLOSED;
        c->last_seg_len = 0;
    }
}

/* TCP handler for IPv6 segments. Called from net.c handle_ipv6(). */
void tcp_handle6(ipv6_addr_t src, ipv6_addr_t dst, uint8_t* data, uint16_t len) {
    (void)dst;  /* IPv6 TCP demux uses src only */
    if (len < sizeof(struct tcp_hdr)) return;
    struct tcp_hdr* h = (struct tcp_hdr*)data;
    uint8_t  data_off = (ntohs16(h->data_off_flags) >> 12) & 0xF;
    uint16_t hdr_len = data_off * 4;
    if (hdr_len < 20 || hdr_len > len) return;
    uint16_t flags = ntohs16(h->data_off_flags) & 0x1FF;
    uint8_t* payload = &data[hdr_len];
    uint16_t payload_len = len - hdr_len;

    uint16_t src_port = ntohs16(h->src_port);
    uint16_t dst_port = ntohs16(h->dst_port);
    uint32_t seg_seq = ntohl32(h->seq);

    if (flags & TCP_RST) {
        struct tcp_conn* c = find_conn6(src, src_port, dst_port);
        if (c) {
            pr_warn("tcp6: RST on conn (slot %d), aborting\n",
                    (int)(c - conns));
            c->state = TCP_CLOSED;
            c->tcp_connect_failed = 1;
            c->last_seg_len = 0;
        }
        return;
    }

    if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
        struct tcp_conn* listener = find_listener(dst_port);
        if (listener) {
            int slot = alloc_conn_slot();
            if (slot < 0) {
                pr_warn("tcp6: no free slots for new connection\n");
                return;
            }
            struct tcp_conn* nc = &conns[slot];
            memset(nc, 0, sizeof(*nc));
            nc->in_use = 1;
            nc->is_ipv6 = 1;
            nc->peer_ip6 = src;
            nc->peer_ip = IP_ZERO;
            nc->peer_port = src_port;
            nc->local_port = dst_port;
            nc->tx_seq = 0x56789u ^ (uint32_t)timer_get_ms();
            nc->rx_seq = seg_seq + 1;
            tcp_send_seg(nc, TCP_SYN | TCP_ACK, NULL, 0);
            nc->state = TCP_SYN_RECEIVED;

            listener->pending_accept = 1;
            listener->accepted = nc;

            char addr_str[40];
            ipv6_addr_to_str(src, addr_str, sizeof(addr_str));
            pr_info("tcp6: SYN from %s:%u -> port %u (slot %d)\n",
                    addr_str, (unsigned)src_port, (unsigned)dst_port, slot);
            return;
        }
        return;
    }

    struct tcp_conn* c = find_conn6(src, src_port, dst_port);
    if (!c) return;

    /* Same state machine as tcp_handle */
    if (c->state == TCP_SYN_SENT && (flags & TCP_SYN) && (flags & TCP_ACK)) {
        c->rx_seq = seg_seq + 1;
        c->tx_seq = ntohl32(h->ack);
        tcp_send_seg(c, TCP_ACK, NULL, 0);
        c->state = TCP_ESTABLISHED;
        c->tcp_connected = 1;
        c->last_seg_len = 0;
        return;
    }

    if (c->state == TCP_SYN_RECEIVED && (flags & TCP_ACK)) {
        c->state = TCP_ESTABLISHED;
        c->last_seg_len = 0;
        return;
    }

    if (c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 ||
        c->state == TCP_FIN_WAIT_2 || c->state == TCP_CLOSE_WAIT) {
        if (seg_seq != c->rx_seq) {
            if (flags & TCP_ACK) {
                tcp_send_seg(c, TCP_ACK, NULL, 0);
            }
            if (!(flags & TCP_FIN)) return;
        }

        if (payload_len > 0 && (flags & (TCP_PSH | TCP_ACK))) {
            if (c->rx_len + payload_len <= (uint16_t)sizeof(c->rx_buf)) {
                memcpy(&c->rx_buf[c->rx_len], payload, payload_len);
                c->rx_len += payload_len;
            }
            c->rx_seq += payload_len;
            tcp_send_seg(c, TCP_ACK, NULL, 0);
        }

        if (flags & TCP_FIN) {
            c->rx_seq += 1;
            tcp_send_seg(c, TCP_ACK, NULL, 0);
            if (c->state == TCP_ESTABLISHED) {
                c->state = TCP_CLOSE_WAIT;
                c->rx_closed = 1;
                c->last_seg_len = 0;
                tcp_send_seg(c, TCP_FIN | TCP_ACK, NULL, 0);
                c->state = TCP_LAST_ACK;
            } else if (c->state == TCP_FIN_WAIT_1) {
                c->state = TCP_CLOSING;
                c->state = TCP_CLOSED;
            } else if (c->state == TCP_FIN_WAIT_2) {
                c->state = TCP_CLOSED;
            }
        }
    }

    if (c->state == TCP_LAST_ACK && (flags & TCP_ACK)) {
        c->state = TCP_CLOSED;
        c->last_seg_len = 0;
    }
}

/* ----- retransmit timer (called from net_tick) ----- */

void tcp_tick(void) {
    uint64_t now = timer_get_ms();
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!conns[i].in_use) continue;
        if (conns[i].state == TCP_CLOSED) {
            conns[i].in_use = 0;
            continue;
        }
        if (conns[i].last_seg_len == 0) continue;

        uint32_t rto = TCP_RETRANSMIT_TIMEOUT_MS;
        for (int j = 0; j < conns[i].retransmit_count && rto < 30000; j++)
            rto *= 2;

        if (now - conns[i].last_seg_time >= rto) {
            if (conns[i].retransmit_count >= TCP_MAX_RETRANSMITS) {
                pr_warn("tcp: max retransmits on slot %d, aborting\n", i);
                conns[i].state = TCP_CLOSED;
                conns[i].tcp_connect_failed = 1;
                conns[i].last_seg_len = 0;
                continue;
            }
            pr_info("tcp: retransmit #%d on slot %d\n",
                    conns[i].retransmit_count + 1, i);
            struct tcp_hdr* rh = (struct tcp_hdr*)conns[i].last_seg;
            rh->checksum = 0;
            if (conns[i].is_ipv6) {
                mac_addr_t mac = net_get_mac();
                ipv6_addr_t src_ll = ipv6_create_link_local(mac);
                rh->checksum = htons16(tcp_checksum6(src_ll, conns[i].peer_ip6,
                                    conns[i].last_seg, conns[i].last_seg_len));
                eth_send_ipv6_pub(conns[i].peer_ip6, 6,
                                  conns[i].last_seg, conns[i].last_seg_len);
            } else {
                rh->checksum = htons16(tcp_checksum(my_ip, conns[i].peer_ip,
                                    conns[i].last_seg, conns[i].last_seg_len));
                eth_send_ipv4_pub(conns[i].peer_ip, 6,
                                  conns[i].last_seg, conns[i].last_seg_len);
            }
            conns[i].last_seg_time = now;
            conns[i].retransmit_count++;
        }
    }
}
