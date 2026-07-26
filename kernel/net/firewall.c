/*
 * Lestra OS - Packet Firewall
 * Copyright (c) 2026 lestramk.org
 *
 * Simple stateless packet filter. Rules are evaluated top-to-bottom;
 * first match wins. If no rule matches, the default policy applies.
 */

#include <lestra/types.h>
#include <lestra/net.h>
#include <lestra/firewall.h>
#include <lestra/printk.h>
#include <string.h>

/* Mirror the private IP header layout from net.c so we can parse fields
 * without exposing them in the public net.h header. */
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

struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  off_flags;
    uint8_t  window;
    uint16_t checksum;
    uint16_t urgent;
} __packed;

struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __packed;

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

/* ----- state ----- */
static struct fw_rule rules[FW_MAX_RULES];
static struct fw_stats stats;
static enum fw_action default_in_policy  = FW_ACCEPT;
static enum fw_action default_out_policy = FW_ACCEPT;
static int fw_initialized = 0;

/* ----- helpers ----- */
static uint32_t ip_to_u32(ipv4_addr_t a) {
    return ((uint32_t)a.bytes[0] << 24) | ((uint32_t)a.bytes[1] << 16) |
           ((uint32_t)a.bytes[2] << 8)  | a.bytes[3];
}

static int ipv4_is_zero_addr(ipv4_addr_t a) {
    return a.bytes[0] == 0 && a.bytes[1] == 0 &&
           a.bytes[2] == 0 && a.bytes[3] == 0;
}

/* ----- init ----- */
void fw_init(void) {
    memset(rules, 0, sizeof(rules));
    memset(&stats, 0, sizeof(stats));
    default_in_policy  = FW_ACCEPT;
    default_out_policy = FW_ACCEPT;
    fw_initialized = 1;
    pr_info("firewall: initialized (default policy: accept in/accept out)\n");
}

/* ----- rule management ----- */
int fw_add_rule(const char* name, enum fw_action action, enum fw_proto proto,
                ipv4_addr_t src_ip, ipv4_addr_t src_mask,
                ipv4_addr_t dst_ip, ipv4_addr_t dst_mask,
                uint16_t src_port, uint16_t dst_port,
                int direction, int logged) {
    if (!fw_initialized) fw_init();

    /* Find empty slot */
    int slot = -1;
    for (int i = 0; i < FW_MAX_RULES; i++) {
        if (!rules[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -1;

    struct fw_rule* r = &rules[slot];
    memset(r, 0, sizeof(*r));
    r->in_use    = 1;
    r->action    = action;
    r->proto     = proto;
    r->src_ip    = src_ip;
    r->src_mask  = src_mask;
    r->dst_ip    = dst_ip;
    r->dst_mask  = dst_mask;
    r->src_port  = src_port;
    r->dst_port  = dst_port;
    r->direction = direction;
    r->logged    = logged;
    r->match_count = 0;

    /* Copy name (truncated to 31 chars) */
    int len = 0;
    while (name[len] && len < 31) { r->name[len] = name[len]; len++; }
    r->name[len] = '\0';

    return slot;
}

int fw_remove_rule(const char* name) {
    for (int i = 0; i < FW_MAX_RULES; i++) {
        if (rules[i].in_use && strcmp(rules[i].name, name) == 0) {
            rules[i].in_use = 0;
            return 0;
        }
    }
    return -1;
}

int fw_remove_rule_by_id(int id) {
    if (id < 0 || id >= FW_MAX_RULES) return -1;
    if (!rules[id].in_use) return -1;
    rules[id].in_use = 0;
    return 0;
}

void fw_list_rules(void) {
    if (!fw_initialized) fw_init();
    printk("\n  %-4s %-18s %-7s %-6s %-5s %-18s %-18s %-6s %-6s %-5s %-8s %s\n",
           "ID", "NAME", "ACTION", "PROTO", "DIR", "SRC", "DST",
           "SPORT", "DPORT", "LOG", "MATCHES", "SENTINEL");
    printk("  ---- ------------------ ------- ------ ----- ------------------ ------------------ ------ ------ ----- -------- --------\n");
    int count = 0;
    for (int i = 0; i < FW_MAX_RULES; i++) {
        if (!rules[i].in_use) continue;
        count++;
        struct fw_rule* r = &rules[i];
        const char* action = (r->action == FW_ACCEPT) ? "ACCEPT" :
                             (r->action == FW_DROP)   ? "DROP"   : "REJECT";
        const char* proto = (r->proto == FW_PROTO_TCP)  ? "TCP"  :
                            (r->proto == FW_PROTO_UDP)  ? "UDP"  :
                            (r->proto == FW_PROTO_ICMP) ? "ICMP" : "ANY";
        const char* dir = (r->direction == 0) ? "IN" :
                          (r->direction == 1) ? "OUT" : "BOTH";

        char src_str[20], dst_str[20];
        if (ipv4_is_zero_addr(r->src_ip)) {
            strcpy(src_str, "*");
        } else {
            uint32_t m = ip_to_u32(r->src_mask);
            if (m == 0xFFFFFFFF) {
                ksnprintf(src_str, sizeof(src_str), "%u.%u.%u.%u",
                         r->src_ip.bytes[0], r->src_ip.bytes[1],
                         r->src_ip.bytes[2], r->src_ip.bytes[3]);
            } else {
                ksnprintf(src_str, sizeof(src_str), "%u.%u.%u.%u/%u.%u.%u.%u",
                         r->src_ip.bytes[0], r->src_ip.bytes[1],
                         r->src_ip.bytes[2], r->src_ip.bytes[3],
                         r->src_mask.bytes[0], r->src_mask.bytes[1],
                         r->src_mask.bytes[2], r->src_mask.bytes[3]);
            }
        }
        if (ipv4_is_zero_addr(r->dst_ip)) {
            strcpy(dst_str, "*");
        } else {
            ksnprintf(dst_str, sizeof(dst_str), "%u.%u.%u.%u",
                     r->dst_ip.bytes[0], r->dst_ip.bytes[1],
                     r->dst_ip.bytes[2], r->dst_ip.bytes[3]);
        }

        printk("  %-4d %-18s %-7s %-6s %-5s %-18s %-18s %-6u %-6u %-5s %-8lu %s\n",
               i, r->name, action, proto, dir, src_str, dst_str,
               (unsigned)r->src_port, (unsigned)r->dst_port,
               r->logged ? "yes" : "no",
               (unsigned long)r->match_count,
               i == FW_MAX_RULES - 1 ? "(last)" : "");
    }
    if (count == 0) {
        printk("  (no rules defined)\n");
    }
    printk("  %d rule%s total\n\n", count, count == 1 ? "" : "s");
}

void fw_flush(void) {
    memset(rules, 0, sizeof(rules));
    printk("firewall: all rules flushed\n");
}

void fw_set_default(int direction, enum fw_action action) {
    if (direction == 0) default_in_policy  = action;
    else                default_out_policy = action;
}

void fw_status(void) {
    if (!fw_initialized) fw_init();
    const char* in_str  = (default_in_policy == FW_ACCEPT) ? "ACCEPT" :
                          (default_in_policy == FW_DROP)   ? "DROP"   : "REJECT";
    const char* out_str = (default_out_policy == FW_ACCEPT) ? "ACCEPT" :
                          (default_out_policy == FW_DROP)   ? "DROP"   : "REJECT";
    int count = 0;
    for (int i = 0; i < FW_MAX_RULES; i++) if (rules[i].in_use) count++;

    printk("\n  Firewall status:\n");
    printk("    Rules:         %d / %d\n", count, FW_MAX_RULES);
    printk("    Default IN:    %s\n", in_str);
    printk("    Default OUT:   %s\n", out_str);
    printk("    Packets IN:    %lu\n", (unsigned long)stats.total_in);
    printk("    Packets OUT:   %lu\n", (unsigned long)stats.total_out);
    printk("    Accepted IN:   %lu\n", (unsigned long)stats.accepted_in);
    printk("    Accepted OUT:  %lu\n", (unsigned long)stats.accepted_out);
    printk("    Dropped IN:    %lu\n", (unsigned long)stats.dropped_in);
    printk("    Dropped OUT:   %lu\n", (unsigned long)stats.dropped_out);
    printk("    Rejected IN:   %lu\n", (unsigned long)stats.rejected_in);
    printk("\n");
}

struct fw_stats fw_get_stats(void) {
    return stats;
}

/* ----- rule matching ----- */
static int rule_matches(const struct fw_rule* rule,
                        const struct ip_hdr* ip,
                        int direction) {
    /* Check direction */
    if (rule->direction != 2 && rule->direction != direction) return 0;

    /* Check protocol */
    if (rule->proto != FW_PROTO_ANY) {
        if (rule->proto == FW_PROTO_TCP  && ip->proto != IP_PROTO_TCP)  return 0;
        if (rule->proto == FW_PROTO_UDP  && ip->proto != IP_PROTO_UDP)  return 0;
        if (rule->proto == FW_PROTO_ICMP && ip->proto != IP_PROTO_ICMP) return 0;
    }

    /* Check source IP (with mask) */
    if (!ipv4_is_zero_addr(rule->src_ip)) {
        uint32_t src    = ip_to_u32(ip->src);
        uint32_t rsrc   = ip_to_u32(rule->src_ip);
        uint32_t mask   = ip_to_u32(rule->src_mask);
        if ((src & mask) != (rsrc & mask)) return 0;
    }

    /* Check destination IP (with mask) */
    if (!ipv4_is_zero_addr(rule->dst_ip)) {
        uint32_t dst    = ip_to_u32(ip->dst);
        uint32_t rdst   = ip_to_u32(rule->dst_ip);
        uint32_t mask   = ip_to_u32(rule->dst_mask);
        if ((dst & mask) != (rdst & mask)) return 0;
    }

    /* Check ports (for TCP/UDP) */
    if (rule->src_port || rule->dst_port) {
        if (ip->proto != IP_PROTO_TCP && ip->proto != IP_PROTO_UDP) return 0;

        uint16_t pkt_sport = 0, pkt_dport = 0;
        /* IP header length from ver_ihl */
        uint8_t ihl = ip->ver_ihl & 0x0F;
        uint16_t ip_hdr_len = ihl * 4;
        /* The ip pointer we receive is a raw pointer to the IP header bytes.
         * The transport header starts ip_hdr_len bytes into the data. To read
         * the port fields we need to peek at the bytes that follow the IP
         * header. Because we only have a pointer to the IP header we compute
         * the offset from its base. */
        const uint8_t* raw = (const uint8_t*)ip;
        const uint8_t* l4  = raw + ip_hdr_len;
        if (ip->proto == IP_PROTO_TCP) {
            const struct tcp_hdr* tcp = (const struct tcp_hdr*)l4;
            pkt_sport = ntohs16(tcp->src_port);
            pkt_dport = ntohs16(tcp->dst_port);
        } else {
            const struct udp_hdr* udp = (const struct udp_hdr*)l4;
            pkt_sport = ntohs16(udp->src_port);
            pkt_dport = ntohs16(udp->dst_port);
        }
        if (rule->src_port && rule->src_port != pkt_sport) return 0;
        if (rule->dst_port && rule->dst_port != pkt_dport) return 0;
    }

    return 1;
}

/* ----- main check function ----- */
enum fw_action fw_check(const void* ip_hdr, int direction) {
    if (!fw_initialized) fw_init();

    const struct ip_hdr* ip = (const struct ip_hdr*)ip_hdr;

    if (direction == 0) stats.total_in++;
    else                stats.total_out++;

    /* Walk rules top-to-bottom; first match wins */
    for (int i = 0; i < FW_MAX_RULES; i++) {
        if (!rules[i].in_use) continue;
        if (rule_matches(&rules[i], ip, direction)) {
            rules[i].match_count++;

            if (rules[i].logged) {
                const char* dstr = (direction == 0) ? "IN" : "OUT";
                const char* action_str = (rules[i].action == FW_ACCEPT) ? "ACCEPT" :
                                         (rules[i].action == FW_DROP)   ? "DROP"   : "REJECT";
                pr_info("firewall: %s rule[%d] %s %s %u.%u.%u.%u -> %u.%u.%u.%u proto=%d\n",
                        action_str, i, rules[i].name, dstr,
                        ip->src.bytes[0], ip->src.bytes[1], ip->src.bytes[2], ip->src.bytes[3],
                        ip->dst.bytes[0], ip->dst.bytes[1], ip->dst.bytes[2], ip->dst.bytes[3],
                        ip->proto);
            }

            if (rules[i].action == FW_ACCEPT) {
                if (direction == 0) stats.accepted_in++;
                else                stats.accepted_out++;
            } else if (rules[i].action == FW_DROP) {
                if (direction == 0) stats.dropped_in++;
                else                stats.dropped_out++;
            } else {
                /* REJECT — counts as inbound drop with ICMP */
                stats.rejected_in++;
            }
            return rules[i].action;
        }
    }

    /* No rule matched — apply default policy */
    enum fw_action dflt = (direction == 0) ? default_in_policy : default_out_policy;
    if (dflt == FW_ACCEPT) {
        if (direction == 0) stats.accepted_in++;
        else                stats.accepted_out++;
    } else if (dflt == FW_DROP) {
        if (direction == 0) stats.dropped_in++;
        else                stats.dropped_out++;
    } else {
        stats.rejected_in++;
    }
    return dflt;
}
