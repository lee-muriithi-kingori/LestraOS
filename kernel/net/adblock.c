/*
 * Lestra OS - DNS-based Adblocker
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Intercepts DNS lookups for known ad/tracking domains and returns
 * 0.0.0.0 (NXDOMAIN-style block). The blocklist is a hardcoded array
 * of domain suffixes — when a DNS query matches, we return 0.0.0.0
 * instead of forwarding the query to the real DNS server.
 *
 * The list is small (focused on the most common ad networks) to keep
 * kernel memory usage reasonable. A full blocklist (like EasyList's
 * 80,000+ entries) would need a hash-table or trie data structure.
 *
 * To toggle: shell command `adblock on` / `adblock off` / `adblock status`
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <string.h>

/* Ad/tracking domain suffixes to block.
 * A query for "ads.example.com" matches if any entry is a suffix.
 * Keep this list curated — too many entries slow down every DNS lookup. */
static const char* adblock_domains[] = {
    /* Google Ads */
    "doubleclick.net",
    "googlesyndication.com",
    "googleadservices.com",
    "googletagservices.com",
    "adservice.google.com",
    /* Facebook */
    "connect.facebook.net",
    "facebook.com/tr",
    /* Amazon */
    "amazon-adsystem.com",
    /* Other major ad networks */
    "ads.yahoo.com",
    "adserver.yahoo.com",
    "adsystem.com",
    "adnxs.com",
    "2mdn.net",
    "pubmatic.com",
    "rubiconproject.com",
    "openx.net",
    "adform.net",
    "criteo.com",
    "criteo.net",
    "taboola.com",
    "outbrain.com",
    "quantserve.com",
    "scorecardresearch.com",
    "moatads.com",
    "adsafeprotected.com",
    "contextweb.com",
    "adroll.com",
    /* Analytics / tracking */
    "google-analytics.com",
    "googletagmanager.com",
    "mixpanel.com",
    "segment.io",
    "amplitude.com",
    "hotjar.com",
    "fullstory.com",
    "chartbeat.com",
    /* Common trackers */
    "branch.io",
    "appsflyer.com",
    "adjust.com",
    "kochava.com",
    /* Malware/coin miners */
    "coinhive.com",
    "coin-hive.com",
    "cryptoloot.com",
    "deepmine.io",
    "webminerpool.com",
    /* Pop-under / redirect */
    "popads.net",
    "popcash.net",
    "propellerads.com",
    "adcash.com",
    "adsterra.com",
};

static const int adblock_count = sizeof(adblock_domains) / sizeof(adblock_domains[0]);
static int adblock_enabled = 1;  /* on by default */

/* Check if a hostname should be blocked.
 * Returns 1 if the hostname matches a blocked suffix, 0 otherwise. */
int adblock_should_block(const char* hostname) {
    if (!adblock_enabled || !hostname) return 0;

    int hlen = strlen(hostname);
    for (int i = 0; i < adblock_count; i++) {
        const char* domain = adblock_domains[i];
        int dlen = strlen(domain);
        if (dlen > hlen) continue;

        /* Check if hostname ends with the blocked domain.
         * Also require that the char before the match is a '.' or the
         * hostname equals the domain exactly (to avoid blocking
         * "notdoubleclick.net" when matching "doubleclick.net"). */
        const char* suffix = hostname + hlen - dlen;
        if (strcmp(suffix, domain) == 0) {
            /* Make sure it's a proper domain boundary */
            if (hlen == dlen || hostname[hlen - dlen - 1] == '.') {
                return 1;
            }
        }
    }
    return 0;
}

int adblock_is_enabled(void) {
    return adblock_enabled;
}

void adblock_set_enabled(int enabled) {
    adblock_enabled = enabled ? 1 : 0;
    pr_info("adblock: %s (%u domains in blocklist)\n",
            adblock_enabled ? "enabled" : "disabled",
            (unsigned)adblock_count);
}

int adblock_get_count(void) {
    return adblock_count;
}
