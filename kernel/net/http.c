/*
 * Lestra OS - HTTP/1.0 client (plain HTTP only, no TLS)
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * One-shot GET/POST over the in-kernel TCP stack. Suitable for:
 *   - Downloading packages from a package mirror
 *   - Talking to a local LLM server (Ollama, llama.cpp, vLLM)
 *   - Talking to a local TLS-terminating proxy that forwards to a cloud
 *     HTTPS API (GLM, Claude, Gemini, OpenAI)
 *
 * NOT suitable for direct connection to HTTPS cloud APIs - those need
 * TLS, which is not implemented. See docs/AI.md for proxy setup.
 */

#include <lestra/types.h>
#include <lestra/net.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <string.h>

/* The kernel doesn't link libc, so we use ksnprintf (defined in
 * kernel/core/printk.c) instead of standard snprintf. */
#define snprintf ksnprintf

/* Parse a URL of the form:
 *     http://host[:port]/path
 *     host[:port]/path      (assumes http://)
 * Returns 0 on success. */
int http_parse_url(const char* url,
                   char* scheme_out, int scheme_sz,
                   char* host_out,  int host_sz,
                   uint16_t* port_out,
                   char* path_out,  int path_sz) {
    const char* p = url;
    int schemelen = 0;
    /* Default scheme = http */
    if (scheme_out && scheme_sz > 0) scheme_out[0] = '\0';

    /* Detect scheme: look for "://" */
    const char* colon = p;
    while (*colon && *colon != ':' && *colon != '/') colon++;
    if (*colon == ':' && colon[1] == '/' && colon[2] == '/') {
        schemelen = (int)(colon - p);
        if (scheme_out && scheme_sz > schemelen + 1) {
            memcpy(scheme_out, p, schemelen);
            scheme_out[schemelen] = '\0';
        }
        p = colon + 3;   /* skip "://" */
    }

    /* Set default port based on scheme — https uses 443 */
    int is_https = 0;
    if (schemelen > 0 && scheme_out && scheme_out[0]) {
        if (strcmp(scheme_out, "https") == 0) is_https = 1;
    } else if (schemelen == 5) {
        /* Fallback if scheme_out buffer was too small: check raw url prefix */
        if (url[0]=='h'&&url[1]=='t'&&url[2]=='t'&&url[3]=='p'&&url[4]=='s') is_https = 1;
    }
    /* Extract host[:port] */
    int hostlen = 0;
    *port_out = is_https ? 443 : 80;
    while (*p && *p != '/' && *p != '\0') {
        if (*p == ':') {
            /* Port number follows */
            p++;
            uint16_t port = 0;
            while (*p >= '0' && *p <= '9') {
                port = port * 10 + (uint16_t)(*p - '0');
                p++;
            }
            *port_out = port;
            break;
        }
        if (hostlen < host_sz - 1) {
            host_out[hostlen++] = *p;
        }
        p++;
    }
    host_out[hostlen] = '\0';

    /* Path: everything from the first '/' to the end. If no slash, use "/" */
    if (*p == '/') {
        int pathlen = 0;
        while (*p && pathlen < path_sz - 1) {
            path_out[pathlen++] = *p++;
        }
        path_out[pathlen] = '\0';
    } else {
        if (path_sz > 1) {
            path_out[0] = '/';
            path_out[1] = '\0';
        }
    }
    return 0;
}

/* Internal: do a single HTTP request and parse the response.
 * `method` is "GET" or "POST".
 * Returns 0 on success (resp->status / resp->body filled in), -1 on error. */
static int http_request(const char* method,
                         const char* url,
                         const char* content_type,
                         const char* body,
                         size_t body_len,
                         struct http_response* resp) {
    if (!net_is_up()) {
        pr_warn("http: network not up\n");
        return -1;
    }
    char scheme[16];
    char host[128];
    uint16_t port;
    char path[256];
    http_parse_url(url, scheme, sizeof(scheme), host, sizeof(host), &port,
                   path, sizeof(path));

    pr_info("http: %s %s:%u%s\n", method, host, (unsigned)port, path);

    /* If scheme is "https", perform a real TLS handshake before
     * sending the request. The existing tls.c now has REAL P-256
     * ECDHE key exchange (see kernel/net/p256.c). After the handshake,
     * we send the HTTP request through tls_send() and read the
     * response through tls_recv(). */
    int use_tls = 0;
    if (strcmp(scheme, "https") == 0) {
        /* Resolve host first — tls_connect needs an IP, not a hostname. */
        ipv4_addr_t tls_ip;
        if (!net_resolve(host, &tls_ip)) {
            pr_warn("http: failed to resolve %s for TLS\n", host);
            return -1;
        }
        extern int tls_connect(ipv4_addr_t ip, uint16_t port, const char* hostname);
        extern void tls_close(void);
        int rc = tls_connect(tls_ip, port, host);
        if (rc <= 0) {
            pr_warn("http: TLS handshake to %s:%u failed (rc=%d)\n",
                    host, (unsigned)port, rc);
            return -1;
        }
        use_tls = 1;
        pr_info("http: TLS established with %s\n", host);
    }

    /* Resolve host (DNS or numeric) */
    ipv4_addr_t ip;
    if (!net_resolve(host, &ip)) {
        pr_warn("http: failed to resolve %s\n", host);
        return -1;
    }
    pr_info("http: resolved %s -> %u.%u.%u.%u\n", host,
            ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3]);

    /* Connect TCP (skip if we're already in a TLS session — tls_connect_to
     * opened the underlying TCP connection itself). */
    if (!use_tls) {
        if (!tcp_connect(ip, port, 5000)) {
            pr_warn("http: tcp_connect failed\n");
            return -1;
        }
    }

    /* Build request */
    static char req[2048];
    int reqlen = 0;
    reqlen += snprintf(&req[reqlen], sizeof(req) - reqlen,
                       "%s %s HTTP/1.0\r\n", method, path);
    reqlen += snprintf(&req[reqlen], sizeof(req) - reqlen,
                       "Host: %s\r\n", host);
    reqlen += snprintf(&req[reqlen], sizeof(req) - reqlen,
                       "Connection: close\r\n");
    reqlen += snprintf(&req[reqlen], sizeof(req) - reqlen,
                       "User-Agent: LestraOS/1.0\r\n");
    if (body && body_len > 0) {
        if (content_type) {
            reqlen += snprintf(&req[reqlen], sizeof(req) - reqlen,
                               "Content-Type: %s\r\n", content_type);
        }
        reqlen += snprintf(&req[reqlen], sizeof(req) - reqlen,
                           "Content-Length: %u\r\n", (unsigned)body_len);
    }
    reqlen += snprintf(&req[reqlen], sizeof(req) - reqlen, "\r\n");
    if (body && body_len > 0) {
        if (reqlen + body_len > sizeof(req) - 1) body_len = sizeof(req) - 1 - reqlen;
        memcpy(&req[reqlen], body, body_len);
        reqlen += (int)body_len;
    }

    /* Send request — over TLS if we're in a TLS session, plain TCP otherwise. */
    if (use_tls) {
        extern int tls_send(const void* data, uint16_t len);
        extern void tls_close(void);
        int rc = tls_send((const void*)req, (uint16_t)reqlen);
        if (rc <= 0) {
            pr_warn("http: tls_send failed\n");
            tls_close();
            return -1;
        }
    } else {
        int sent = 0;
        while (sent < reqlen) {
            int n = tcp_send(&req[sent], (uint16_t)(reqlen - sent));
            if (n <= 0) {
                pr_warn("http: tcp_send failed\n");
                tcp_close();
                return -1;
            }
            sent += n;
        }
    }

    /* Receive response (until peer closes or buffer fills) */
    static uint8_t rbuf[16384];
    uint16_t total = 0;
    if (use_tls) {
        extern int tls_recv(void* buf, uint16_t bufsz, uint32_t timeout_ms);
        extern void tls_close(void);
        uint32_t timeout = 8000;
        while (total < sizeof(rbuf) - 1) {
            int n = tls_recv(&rbuf[total], (uint16_t)(sizeof(rbuf) - 1 - total), timeout);
            if (n <= 0) break;
            total += (uint16_t)n;
            timeout = 1500;
        }
        tls_close();
    } else {
        uint32_t timeout = 8000;
        while (total < sizeof(rbuf) - 1) {
            int n = tcp_recv_wait(&rbuf[total], (uint16_t)(sizeof(rbuf) - 1 - total), timeout);
            if (n <= 0) break;
            total += (uint16_t)n;
            timeout = 1500;
        }
        tcp_close();
    }
    rbuf[total] = '\0';

    if (total == 0) {
        pr_warn("http: no response data\n");
        return -1;
    }

    /* Parse status line: "HTTP/1.0 200 OK\r\n..." */
    if (total < 12 || rbuf[0] != 'H' || rbuf[1] != 'T' || rbuf[2] != 'T' || rbuf[3] != 'P') {
        pr_warn("http: malformed response\n");
        return -1;
    }
    /* Find first space, then parse 3-digit status */
    int i = 0;
    while (i < total && rbuf[i] != ' ') i++;
    if (i + 4 > total) return -1;
    resp->status = ((rbuf[i+1]-'0') * 100) + ((rbuf[i+2]-'0') * 10) + (rbuf[i+3]-'0');

    /* Find end of headers (\r\n\r\n) */
    int body_start = -1;
    for (int j = 0; j + 3 < total; j++) {
        if (rbuf[j] == '\r' && rbuf[j+1] == '\n' &&
            rbuf[j+2] == '\r' && rbuf[j+3] == '\n') {
            body_start = j + 4;
            break;
        }
    }
    if (body_start < 0) {
        /* No body separator - treat whole thing as body */
        body_start = 0;
    }
    size_t body_size = total - body_start;
    if (body_size > sizeof(resp->body) - 1) body_size = sizeof(resp->body) - 1;
    memcpy(resp->body, &rbuf[body_start], body_size);
    resp->body[body_size] = '\0';
    resp->body_len = body_size;

    pr_info("http: response %u, %u bytes body\n",
            (unsigned)resp->status, (unsigned)body_size);
    return 0;
}

int http_get(const char* url, struct http_response* resp) {
    return http_request("GET", url, NULL, NULL, 0, resp);
}

int http_post(const char* url,
              const char* content_type,
              const char* body,
              size_t body_len,
              struct http_response* resp) {
    return http_request("POST", url, content_type, body, body_len, resp);
}
