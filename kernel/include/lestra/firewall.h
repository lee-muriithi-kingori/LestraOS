/*
 * Lestra OS - Packet Firewall
 * Copyright (c) 2026 lestramk.org
 *
 * Stateful packet filter with per-rule statistics and configurable
 * default policies for inbound and outbound traffic.
 */

#ifndef LESTRA_FIREWALL_H
#define LESTRA_FIREWALL_H

#include <lestra/types.h>
#include <lestra/net.h>

#define FW_MAX_RULES 32

enum fw_action {
    FW_ACCEPT = 0,
    FW_DROP,
    FW_REJECT,
};

enum fw_proto {
    FW_PROTO_ANY = 0,
    FW_PROTO_TCP,
    FW_PROTO_UDP,
    FW_PROTO_ICMP,
};

struct fw_rule {
    int in_use;
    char name[32];
    enum fw_action action;
    enum fw_proto proto;
    ipv4_addr_t src_ip;
    ipv4_addr_t src_mask;
    ipv4_addr_t dst_ip;
    ipv4_addr_t dst_mask;
    uint16_t src_port;
    uint16_t dst_port;
    int direction;          /* 0=in, 1=out, 2=both */
    int logged;
    uint64_t match_count;
};

struct fw_stats {
    uint64_t total_in;
    uint64_t total_out;
    uint64_t dropped_in;
    uint64_t dropped_out;
    uint64_t rejected_in;
    uint64_t accepted_in;
    uint64_t accepted_out;
};

void fw_init(void);

/* Add a fully-specified rule. Returns rule index (0..FW_MAX_RULES-1) or -1. */
int fw_add_rule(const char* name, enum fw_action action, enum fw_proto proto,
                ipv4_addr_t src_ip, ipv4_addr_t src_mask,
                ipv4_addr_t dst_ip, ipv4_addr_t dst_mask,
                uint16_t src_port, uint16_t dst_port,
                int direction, int logged);

int  fw_remove_rule(const char* name);
int  fw_remove_rule_by_id(int id);
void fw_list_rules(void);
void fw_flush(void);
void fw_set_default(int direction, enum fw_action action);
void fw_status(void);

/* Called from the packet path. direction: 0=in, 1=out */
enum fw_action fw_check(const void* ip_hdr, int direction);

/* Statistics (read-only from outside) */
struct fw_stats fw_get_stats(void);

#endif /* LESTRA_FIREWALL_H */
