/*
 * Lestra OS - Minimal TCP state machine for one-shot HTTP requests
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * This is intentionally NOT a general-purpose TCP. It supports exactly
 * one outstanding connection at a time, in a strict request/response
 * pattern:
 *
 *   1. tcp_connect(dst_ip, dst_port)
 *   2. tcp_send(data, len)         (writes happen via one or more PSH segments)
 *   3. tcp_recv(buf, bufsz, timeout)  (collects data until FIN or timeout)
 *   4. tcp_close()                  (sends FIN, transitions to CLOSED)
 *
 * The state machine is advanced by net_tick() calling tcp_tick(), which
 * processes any incoming TCP packets (already demuxed by net.c) and
 * retransmits if needed.
 *
 * Supported states: CLOSED, SYN_SENT, ESTABLISHED, CLOSE_WAIT, LAST_ACK.
 * We deliberately don't implement TIME_WAIT (just go straight to CLOSED
 * after sending the final ACK) — this is fine for a client that does
 * one request at a time.
 */

#include <lestra/types.h>
#include <lestra/net.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <string.h>

/* IP header struct (defined in net.c — duplicate the layout here so we
 * can avoid leaking net.c's private header into tcp.c) */
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

/* TCP header */
struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint16_t data_off_flags;   /* high 4 bits = data offset (in 32-bit words), low 12 = flags */
    uint16_t window;
    uint16_t checksum;
    uint16_t urg_ptr;
} __packed;

/* TCP flags */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

/* Pseudo-header for TCP checksum */
struct tcp_pseudo {
    ipv4_addr_t src;
    ipv4_addr_t dst;
    uint8_t  zero;
    uint8_t  proto;
    uint16_t tcp_len;
} __packed;

/* TCP connection states */
typedef enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_CLOSING,
} tcp_state_t;

static tcp_state_t tcp_state = TCP_CLOSED;
static ipv4_addr_t tcp_peer_ip;
static uint16_t    tcp_peer_port;
static uint16_t    tcp_local_port = 0x4000;
static uint32_t    tx_seq = 0;
static uint32_t    rx_seq = 0;

/* Receive buffer for incoming data */
#define TCP_RX_BUF 8192
static uint8_t  tcp_rx_buf[TCP_RX_BUF];
static uint16_t tcp_rx_len = 0;
static int      tcp_rx_closed = 0;   /* set when peer sends FIN */

/* Connection completion flag (for SYN_SENT -> ESTABLISHED) */
static int      tcp_connected = 0;
static int      tcp_connect_failed = 0;

/* Extern from net.c */
extern int eth_send_ipv4_pub(ipv4_addr_t dst_ip, uint8_t proto,
                              const void* payload, uint16_t payload_len);
extern ipv4_addr_t my_ip;   /* our own IP, for TCP checksum */

/* Compute TCP checksum (pseudo-header + TCP segment) */
static uint16_t tcp_checksum(ipv4_addr_t src, ipv4_addr_t dst,
                              const void* seg, uint16_t seg_len) {
    struct tcp_pseudo ph;
    ph.src = src;
    ph.dst = dst;
    ph.zero = 0;
    ph.proto = 6;
    ph.tcp_len = htons16(seg_len);

    /* Sum pseudo-header then segment */
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

/* Build and send a TCP segment. `flags` is the OR of TCP_* above.
 * `payload` may be NULL if payload_len == 0. */
static int tcp_send_seg(uint8_t flags, const void* payload, uint16_t payload_len) {
    uint8_t buf[sizeof(struct tcp_hdr) + 1500];
    struct tcp_hdr* h = (struct tcp_hdr*)buf;
    h->src_port = htons16(tcp_local_port);
    h->dst_port = htons16(tcp_peer_port);
    h->seq      = htonl32(tx_seq);
    h->ack      = htonl32(rx_seq);
    h->data_off_flags = htons16((5 << 12) | flags);   /* 5 = 20-byte header, no options */
    h->window   = htons16(8192);
    h->checksum = 0;
    h->urg_ptr  = 0;
    if (payload_len && payload) {
        memcpy(&buf[sizeof(struct tcp_hdr)], payload, payload_len);
    }
    uint16_t seg_len = sizeof(struct tcp_hdr) + payload_len;
    h->checksum = htons16(tcp_checksum(my_ip, tcp_peer_ip, buf, seg_len));

    int r = eth_send_ipv4_pub(tcp_peer_ip, 6 /* TCP */, buf, seg_len);
    /* Advance our seq by payload bytes + 1 if SYN or FIN (they consume a seq) */
    tx_seq += payload_len;
    if (flags & (TCP_SYN | TCP_FIN)) tx_seq += 1;
    return r;
}

/* ----- public API ----- */

int tcp_connect(ipv4_addr_t dst_ip, uint16_t dst_port, uint32_t timeout_ms) {
    if (tcp_state != TCP_CLOSED) return 0;
    if (!net_is_up()) return 0;

    tcp_peer_ip = dst_ip;
    tcp_peer_port = dst_port;
    tcp_local_port++;
    if (tcp_local_port < 0x4000) tcp_local_port = 0x4000;
    tx_seq = 0x12345u ^ (uint32_t)timer_get_ms();
    rx_seq = 0;
    tcp_rx_len = 0;
    tcp_rx_closed = 0;
    tcp_connected = 0;
    tcp_connect_failed = 0;

    /* Send SYN */
    if (!tcp_send_seg(TCP_SYN, NULL, 0)) return 0;
    tcp_state = TCP_SYN_SENT;

    /* Wait for SYN-ACK (handled by tcp_handle) */
    uint64_t deadline = timer_get_ms() + timeout_ms;
    while (timer_get_ms() < deadline) {
        if (tcp_connected) return 1;
        if (tcp_connect_failed) return 0;
        net_tick();
    }
    pr_warn("tcp_connect: timed out waiting for SYN-ACK\n");
    tcp_state = TCP_CLOSED;
    return 0;
}

int tcp_send(const void* data, uint16_t len) {
    if (tcp_state != TCP_ESTABLISHED) return 0;
    /* Send as one PSH+ACK segment. (For large payloads we'd fragment, but
     * 8 KB fits in one Ethernet frame at MTU 1500 only if len <= ~1460.
     * For simplicity we cap at 1400 bytes per send; HTTP requests are
     * usually small.) */
    if (len > 1400) len = 1400;
    return tcp_send_seg(TCP_PSH | TCP_ACK, data, len);
}

int tcp_recv_wait(uint8_t* buf, uint16_t bufsz, uint32_t timeout_ms) {
    uint64_t deadline = timer_get_ms() + timeout_ms;
    uint16_t copied = 0;
    while (timer_get_ms() < deadline) {
        if (tcp_rx_len > 0) {
            uint16_t n = tcp_rx_len;
            if (n > bufsz - copied) n = bufsz - copied;
            memcpy(&buf[copied], tcp_rx_buf, n);
            /* Shift remaining data in the rx buffer */
            memmove(tcp_rx_buf, &tcp_rx_buf[n], tcp_rx_len - n);
            tcp_rx_len -= n;
            copied += n;
        }
        if (tcp_rx_closed) {
            return copied;
        }
        if (copied >= bufsz) return copied;
        net_tick();
    }
    return copied;
}

void tcp_close(void) {
    if (tcp_state == TCP_ESTABLISHED) {
        /* Send FIN+ACK */
        tcp_send_seg(TCP_FIN | TCP_ACK, NULL, 0);
        tcp_state = TCP_FIN_WAIT_1;
    } else if (tcp_state == TCP_CLOSE_WAIT) {
        /* Peer already FIN'd; send our FIN */
        tcp_send_seg(TCP_FIN | TCP_ACK, NULL, 0);
        tcp_state = TCP_LAST_ACK;
    } else {
        tcp_state = TCP_CLOSED;
    }
}

int tcp_is_closed(void) {
    return tcp_state == TCP_CLOSED;
}

/* ----- inbound packet handler (called from net.c) ----- */
void tcp_handle(struct ip_hdr_pub* ip_pub, uint8_t* data, uint16_t len) {
    if (len < sizeof(struct tcp_hdr)) return;
    /* Cast through void* to avoid struct aliasing warnings */
    struct ip_hdr_pub ip = *ip_pub;
    struct tcp_hdr* h = (struct tcp_hdr*)data;
    uint8_t  data_off = (ntohs16(h->data_off_flags) >> 12) & 0xF;
    uint16_t hdr_len = data_off * 4;
    if (hdr_len < 20 || hdr_len > len) return;
    uint16_t flags = ntohs16(h->data_off_flags) & 0x1FF;
    uint8_t* payload = &data[hdr_len];
    uint16_t payload_len = len - hdr_len;

    /* Only handle packets from our current peer (or while waiting in SYN_SENT) */
    if (!ipv4_eq(ip.src, tcp_peer_ip)) return;
    if (ntohs16(h->src_port) != tcp_peer_port) return;
    if (ntohs16(h->dst_port) != tcp_local_port) return;

    /* If RST, abort the connection */
    if (flags & TCP_RST) {
        pr_warn("tcp: RST received, aborting connection\n");
        tcp_state = TCP_CLOSED;
        tcp_connect_failed = 1;
        return;
    }

    if (tcp_state == TCP_SYN_SENT && (flags & TCP_SYN) && (flags & TCP_ACK)) {
        /* SYN-ACK: finalize the handshake */
        rx_seq = ntohl32(h->seq) + 1;  /* peer's SYN consumes one seq */
        tx_seq = ntohl32(h->ack);       /* our seq is now what peer ACK'd */
        /* Send ACK */
        tcp_send_seg(TCP_ACK, NULL, 0);
        tcp_state = TCP_ESTABLISHED;
        tcp_connected = 1;
        return;
    }

    if (tcp_state == TCP_ESTABLISHED || tcp_state == TCP_FIN_WAIT_1 ||
        tcp_state == TCP_FIN_WAIT_2 || tcp_state == TCP_CLOSE_WAIT) {
        /* Track the peer's sequence */
        uint32_t seg_seq = ntohl32(h->seq);
        if (seg_seq != rx_seq) {
            /* Out-of-order or retransmit. For our simple stack, drop the
             * payload but still ACK to nudge the peer. */
            if (flags & TCP_ACK) {
                /* Send a duplicate ACK */
                uint32_t saved_tx = tx_seq;
                tcp_send_seg(TCP_ACK, NULL, 0);
                (void)saved_tx;
            }
            /* If FIN was set, we still need to handle it. */
            if (!(flags & TCP_FIN)) return;
        }

        /* Accept payload */
        if (payload_len > 0 && (flags & (TCP_PSH | TCP_ACK))) {
            if (tcp_rx_len + payload_len <= TCP_RX_BUF) {
                memcpy(&tcp_rx_buf[tcp_rx_len], payload, payload_len);
                tcp_rx_len += payload_len;
            }
            rx_seq += payload_len;
            /* ACK the data */
            tcp_send_seg(TCP_ACK, NULL, 0);
        }

        /* Handle FIN */
        if (flags & TCP_FIN) {
            rx_seq += 1;  /* FIN consumes one seq */
            tcp_send_seg(TCP_ACK, NULL, 0);
            if (tcp_state == TCP_ESTABLISHED) {
                tcp_state = TCP_CLOSE_WAIT;
                tcp_rx_closed = 1;
                /* We'll send our own FIN when tcp_close() is called */
                tcp_send_seg(TCP_FIN | TCP_ACK, NULL, 0);
                tcp_state = TCP_LAST_ACK;
            } else if (tcp_state == TCP_FIN_WAIT_1) {
                tcp_state = TCP_CLOSING;  /* not implemented, just close */
                tcp_state = TCP_CLOSED;
            } else if (tcp_state == TCP_FIN_WAIT_2) {
                tcp_state = TCP_CLOSED;
            }
        }
    }

    if (tcp_state == TCP_LAST_ACK && (flags & TCP_ACK)) {
        /* Our FIN was ACK'd - we're done */
        tcp_state = TCP_CLOSED;
    }
}

/* Called from net_tick() to advance retransmit timers (currently a no-op
 * because we rely on the peer to retransmit; we'll add real retransmits
 * if/when needed). */
void tcp_tick(void) {
    /* nothing for now */
}
