/*
 * Lestra OS - Network Configuration for VPS/Cloud
 * Copyright (c) 2026 lestramk.org
 *
 * Provides static IP configuration for environments where DHCP is
 * unavailable (VPS, cloud instances, dedicated servers). Also supports
 * DNS configuration and a DHCP fallback mode.
 *
 * Configuration is stored in a static struct and applied during
 * net_init(). If static config is set, it overrides DHCP.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/net.h>
#include <string.h>

/* Configuration modes */
#define NET_CFG_DHCP     0
#define NET_CFG_STATIC   1

struct net_config {
    int mode;                   /* NET_CFG_DHCP or NET_CFG_STATIC */
    ipv4_addr_t ip;
    ipv4_addr_t mask;
    ipv4_addr_t gw;
    ipv4_addr_t dns1;
    ipv4_addr_t dns2;
    int dns_set;                /* 1 if DNS was explicitly configured */
};

static struct net_config cfg = {
    .mode = NET_CFG_DHCP,
    .dns_set = 0,
};

/* Parse a dotted-quad IPv4 string "a.b.c.d" into an ipv4_addr_t.
 * Returns 1 on success, 0 on failure. */
static int parse_ip(const char* str, ipv4_addr_t* out) {
    if (!str || !out) return 0;
    int vals[4] = {0, 0, 0, 0};
    int vi = 0;
    const char* s = str;
    while (*s && vi < 4) {
        if (*s >= '0' && *s <= '9') {
            vals[vi] = vals[vi] * 10 + (*s - '0');
            if (vals[vi] > 255) return 0;
            s++;
        } else if (*s == '.') {
            vi++;
            s++;
        } else {
            return 0;
        }
    }
    if (vi != 3 || *s != '\0') return 0;
    out->bytes[0] = (uint8_t)vals[0];
    out->bytes[1] = (uint8_t)vals[1];
    out->bytes[2] = (uint8_t)vals[2];
    out->bytes[3] = (uint8_t)vals[3];
    return 1;
}

/* Public API: set static IP configuration */
int net_config_set_ip(const char* ip, const char* mask, const char* gw) {
    if (!ip || !mask || !gw) return -1;
    if (!parse_ip(ip, &cfg.ip)) {
        pr_err("net_config: invalid IP '%s'\n", ip);
        return -1;
    }
    if (!parse_ip(mask, &cfg.mask)) {
        pr_err("net_config: invalid mask '%s'\n", mask);
        return -1;
    }
    if (!parse_ip(gw, &cfg.gw)) {
        pr_err("net_config: invalid gateway '%s'\n", gw);
        return -1;
    }
    cfg.mode = NET_CFG_STATIC;
    pr_info("net_config: static IP %s mask %s gw %s\n", ip, mask, gw);
    return 0;
}

/* Public API: set DNS servers */
int net_config_set_dns(const char* dns1, const char* dns2) {
    if (!dns1) return -1;
    if (!parse_ip(dns1, &cfg.dns1)) {
        pr_err("net_config: invalid DNS1 '%s'\n", dns1);
        return -1;
    }
    if (dns2 && *dns2) {
        if (!parse_ip(dns2, &cfg.dns2)) {
            pr_err("net_config: invalid DNS2 '%s'\n", dns2);
            return -1;
        }
    } else {
        cfg.dns2 = IP_ZERO;
    }
    cfg.dns_set = 1;
    pr_info("net_config: DNS1 %s DNS2 %s\n", dns1,
            (dns2 && *dns2) ? dns2 : "(none)");
    return 0;
}

/* Public API: switch to DHCP mode (default) */
void net_config_dhcp(void) {
    cfg.mode = NET_CFG_DHCP;
    cfg.dns_set = 0;
    pr_info("net_config: DHCP mode\n");
}

/* Called from net_init() after the initial setup. If static config is
 * active, overrides whatever DHCP would have set. The net.c statics
 * (my_ip, my_mask, my_gw, my_dns) are written directly via the
 * extern'd ipv4_addr_t my_ip and setter functions. */
void net_config_apply(void) {
    if (cfg.mode != NET_CFG_STATIC) return;

    /* Apply static configuration by writing to net.c's globals */
    extern ipv4_addr_t my_ip;
    extern void net_set_mask(ipv4_addr_t mask);
    extern void net_set_gw(ipv4_addr_t gw);
    extern void net_set_dns(ipv4_addr_t dns);

    my_ip = cfg.ip;
    net_set_mask(cfg.mask);
    net_set_gw(cfg.gw);
    if (cfg.dns_set) {
        net_set_dns(cfg.dns1);
    }

    pr_info("net_config: applied static config\n");
    printk("Network: static IP %u.%u.%u.%u\n",
           cfg.ip.bytes[0], cfg.ip.bytes[1],
           cfg.ip.bytes[2], cfg.ip.bytes[3]);
}

/* Query API */
int  net_config_is_static(void) { return cfg.mode == NET_CFG_STATIC; }
ipv4_addr_t net_config_get_ip(void)   { return cfg.ip; }
ipv4_addr_t net_config_get_mask(void) { return cfg.mask; }
ipv4_addr_t net_config_get_gw(void)   { return cfg.gw; }
ipv4_addr_t net_config_get_dns(void)  { return cfg.dns1; }
