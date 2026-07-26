/*
 * Lestra OS - Built-in HTTP/1.0 server
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Serves files from the VFS root over plain HTTP. Designed for
 * embedded web UIs, status pages, and local tooling.
 */

#include <lestra/types.h>
#include <lestra/net.h>
#include <lestra/vfs.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <lestra/mm.h>
#include <string.h>

#define snprintf ksnprintf

#define HTTP_MAX_CONNS 4

struct http_conn {
    struct tcp_conn* tcp;
    int active;
    char req[2048];
    int req_len;
};

static int http_listen_idx = -1;
static struct http_conn http_conns[HTTP_MAX_CONNS];

static const char* guess_content_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++;
    if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0) return "text/html";
    if (strcmp(ext, "css") == 0) return "text/css";
    if (strcmp(ext, "js") == 0) return "application/javascript";
    if (strcmp(ext, "json") == 0) return "application/json";
    if (strcmp(ext, "txt") == 0 || strcmp(ext, "log") == 0) return "text/plain";
    if (strcmp(ext, "png") == 0) return "image/png";
    if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, "gif") == 0) return "image/gif";
    if (strcmp(ext, "svg") == 0) return "image/svg+xml";
    if (strcmp(ext, "ico") == 0) return "image/x-icon";
    if (strcmp(ext, "xml") == 0) return "text/xml";
    if (strcmp(ext, "pdf") == 0) return "application/pdf";
    if (strcmp(ext, "wasm") == 0) return "application/wasm";
    return "application/octet-stream";
}

static void http_serve_file(struct tcp_conn* c, const char* path) {
    struct stat st;
    if (vfs_stat(path, &st) < 0 || st.size == 0) {
        const char* resp =
            "HTTP/1.0 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 14\r\n"
            "Connection: close\r\n"
            "\r\n"
            "404 Not Found\n";
        tcp_send_conn(c, resp, (uint16_t)strlen(resp));
        return;
    }

    const char* ct = guess_content_type(path);
    char header[256];
    int hlen = ksnprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n",
        ct, (unsigned)st.size);

    tcp_send_conn(c, header, (uint16_t)hlen);

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) return;

    uint8_t chunk[1400];
    uint64_t offset = 0;
    while (offset < st.size) {
        size_t to_read = st.size - offset;
        if (to_read > sizeof(chunk)) to_read = sizeof(chunk);
        ssize_t n = vfs_read(fd, chunk, to_read);
        if (n <= 0) break;
        tcp_send_conn(c, chunk, (uint16_t)n);
        offset += n;
    }
    vfs_close(fd);
}

static void http_handle_request(struct http_conn* hc) {
    char* req = hc->req;
    int len = hc->req_len;

    if (len < 4) return;
    req[len] = '\0';

    char method[8] = {0};
    char path[256] = {0};
    int mi = 0, pi = 0;

    int i = 0;
    while (i < len && req[i] != ' ' && mi < 7) method[mi++] = req[i++];
    i++;
    while (i < len && req[i] != ' ' && req[i] != '\r' && pi < 255) path[pi++] = req[i++];

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        const char* resp =
            "HTTP/1.0 405 Method Not Allowed\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 22\r\n"
            "Connection: close\r\n"
            "\r\n"
            "405 Method Not Allowed\n";
        tcp_send_conn(hc->tcp, resp, (uint16_t)strlen(resp));
        return;
    }

    if (pi == 0 || path[0] != '/') {
        path[0] = '/';
        path[1] = '\0';
    }

    pr_info("http_server: %s %s\n", method, path);

    char vfs_path[256];
    ksnprintf(vfs_path, sizeof(vfs_path), "%s", path);

    http_serve_file(hc->tcp, vfs_path);
}

void http_server_start(uint16_t port) {
    http_listen_idx = tcp_listen(port, HTTP_MAX_CONNS);
    memset(http_conns, 0, sizeof(http_conns));
    if (http_listen_idx >= 0)
        pr_info("http_server: listening on port %u\n", (unsigned)port);
    else
        pr_warn("http_server: failed to listen on port %u\n", (unsigned)port);
}

void http_server_tick(void) {
    if (http_listen_idx < 0) return;

    struct tcp_conn* lc = tcp_get_conn(http_listen_idx);
    if (!lc) return;

    if (lc->pending_accept && lc->accepted) {
        struct tcp_conn* ac = lc->accepted;
        lc->pending_accept = 0;
        lc->accepted = NULL;

        int slot = -1;
        for (int i = 0; i < HTTP_MAX_CONNS; i++) {
            if (!http_conns[i].active) { slot = i; break; }
        }
        if (slot >= 0) {
            http_conns[slot].tcp = ac;
            http_conns[slot].active = 1;
            http_conns[slot].req_len = 0;
            pr_info("http_server: new client on slot %d\n", slot);
        } else {
            pr_warn("http_server: connection slots full, dropping\n");
            tcp_close_conn(ac);
        }
    }

    for (int i = 0; i < HTTP_MAX_CONNS; i++) {
        if (!http_conns[i].active) continue;
        struct http_conn* hc = &http_conns[i];
        struct tcp_conn* tc = hc->tcp;

        if (!tc->in_use || tc->state == TCP_CLOSED) {
            hc->active = 0;
            continue;
        }

        if (tc->rx_len > 0) {
            int space = (int)sizeof(hc->req) - hc->req_len - 1;
            if (space > 0 && tc->rx_len > 0) {
                int n = tc->rx_len;
                if (n > space) n = space;
                memcpy(&hc->req[hc->req_len], tc->rx_buf, n);
                hc->req_len += n;
                memmove(tc->rx_buf, &tc->rx_buf[n], tc->rx_len - n);
                tc->rx_len -= n;
            }

            int complete = 0;
            for (int j = 0; j + 3 < hc->req_len; j++) {
                if (hc->req[j] == '\r' && hc->req[j+1] == '\n' &&
                    hc->req[j+2] == '\r' && hc->req[j+3] == '\n') {
                    complete = 1;
                    break;
                }
            }

            if (complete) {
                http_handle_request(hc);
                tcp_close_conn(tc);
                hc->active = 0;
            }
        }

        if (tc->rx_closed && !hc->active) {
            hc->active = 0;
        }
    }
}
