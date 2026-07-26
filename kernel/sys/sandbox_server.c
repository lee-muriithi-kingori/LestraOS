/*
 * Lestra OS - Sandbox HTTP Server
 * Copyright (c) 2026 lestramk.org
 *
 * An HTTP server that manages sandboxes via a REST API and serves
 * a web UI. Uses the TCP server API (to be wired up later).
 *
 * Endpoints:
 *   GET  /                           — Web UI
 *   GET  /api/sandboxes              — List sandboxes
 *   POST /api/sandbox/create         — Create sandbox (JSON body)
 *   POST /api/sandbox/:id/start      — Start command in sandbox
 *   POST /api/sandbox/:id/stop       — Stop sandbox
 *   DELETE /api/sandbox/:id          — Destroy sandbox
 *   GET  /api/sandbox/:id/output     — Get sandbox output
 *   POST /api/sandbox/:id/exec       — Execute command in sandbox
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/sandbox.h>
#include <lestra/timer.h>
#include <string.h>

static int server_running = 0;
static int server_port    = 8080;
static int server_socket  = -1;

/* Output buffer per sandbox for capturing stdout */
#define OUTPUT_BUF_SIZE 4096
static char output_buf[SANDBOX_MAX][OUTPUT_BUF_SIZE];
static int  output_len[SANDBOX_MAX];

/* ----- HTTP response builder ------------------------------------------- */

static int http_response_build(char* buf, int bufsize,
                               int status, const char* status_text,
                               const char* content_type,
                               const char* body, int body_len) {
    int off = 0;
    off += ksnprintf(buf + off, bufsize - off,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: close\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "\r\n",
                     status, status_text,
                     content_type, body_len);
    if (off < bufsize && body && body_len > 0) {
        int copy = body_len;
        if (off + copy > bufsize - 1) copy = bufsize - 1 - off;
        memcpy(buf + off, body, copy);
        off += copy;
    }
    return off;
}

static int http_ok(char* buf, int bufsize, const char* body) {
    return http_response_build(buf, bufsize, 200, "OK",
                               "application/json", body, (int)strlen(body));
}

static int http_created(char* buf, int bufsize, const char* body) {
    return http_response_build(buf, bufsize, 201, "Created",
                               "application/json", body, (int)strlen(body));
}

static int http_bad_request(char* buf, int bufsize, const char* msg) {
    return http_response_build(buf, bufsize, 400, "Bad Request",
                               "text/plain", msg, (int)strlen(msg));
}

static int http_not_found(char* buf, int bufsize) {
    const char* msg = "Not Found";
    return http_response_build(buf, bufsize, 404, "Not Found",
                               "text/plain", msg, (int)strlen(msg));
}

static int http_error(char* buf, int bufsize, int code, const char* msg) {
    return http_response_build(buf, bufsize, code, "Error",
                               "text/plain", msg, (int)strlen(msg));
}

/* ----- Request parser -------------------------------------------------- */

struct http_request {
    char method[8];
    char path[128];
    char body[2048];
    int  body_len;
};

/* Simple parser for "METHOD /path HTTP/1.x\r\n...\r\n\r\nbody" */
static int parse_http_request(const char* raw, int raw_len,
                              struct http_request* req) {
    memset(req, 0, sizeof(*req));
    const char* p = raw;

    /* Parse method */
    int mi = 0;
    while (*p && *p != ' ' && mi < 7) {
        req->method[mi++] = *p++;
    }
    req->method[mi] = '\0';

    /* Skip space */
    while (*p == ' ') p++;

    /* Parse path */
    int pi = 0;
    while (*p && *p != ' ' && *p != '\r' && pi < 127) {
        req->path[pi++] = *p++;
    }
    req->path[pi] = '\0';

    /* Find end of headers (\r\n\r\n) */
    const char* body_start = NULL;
    for (const char* scan = p; scan < raw + raw_len - 3; scan++) {
        if (scan[0] == '\r' && scan[1] == '\n' &&
            scan[2] == '\r' && scan[3] == '\n') {
            body_start = scan + 4;
            break;
        }
    }

    /* Copy body */
    if (body_start) {
        int max_body = (int)sizeof(req->body) - 1;
        int blen = (int)(raw + raw_len - body_start);
        if (blen > max_body) blen = max_body;
        memcpy(req->body, body_start, blen);
        req->body[blen] = '\0';
        req->body_len = blen;
    }

    return 0;
}

/* Extract the integer after /api/sandbox/ in the path.
 * Returns -1 if the path doesn't match. */
static int extract_sandbox_id(const char* path) {
    const char* prefix = "/api/sandbox/";
    int plen = (int)strlen(prefix);
    if (strncmp(path, prefix, plen) != 0) return -1;
    const char* num = path + plen;
    if (*num < '1' || *num > '2') return -1;
    int id = *num - '0';
    return id;
}

/* Check if path ends with a given suffix */
static int path_ends_with(const char* path, const char* suffix) {
    int plen = (int)strlen(path);
    int slen = (int)strlen(suffix);
    if (slen > plen) return 0;
    return strcmp(path + plen - slen, suffix) == 0;
}

/* ----- Web UI (minimal HTML) ------------------------------------------ */

static const char* web_ui_html =
    "<!DOCTYPE html><html><head>"
    "<meta charset=\"utf-8\"><title>LestraOS Sandbox Manager</title>"
    "<style>"
    "body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;margin:2em;}"
    "h1{color:#00d4ff;} h2{color:#7c4dff;}"
    ".box{border:1px solid #333;padding:1em;margin:1em 0;border-radius:4px;}"
    ".running{border-color:#00e676;} .stopped{border-color:#ff5252;}"
    "button{background:#7c4dff;color:#fff;border:none;padding:.5em 1em;"
    "margin:.3em;cursor:pointer;border-radius:3px;font-family:monospace;}"
    "button:hover{background:#651fff;}"
    "input{background:#0d0d1a;color:#e0e0e0;border:1px solid #333;"
    "padding:.4em;font-family:monospace;}"
    "#status{margin-top:1em;padding:1em;background:#0d0d1a;max-height:300px;"
    "overflow-y:auto;white-space:pre-wrap;}"
    "</style></head><body>"
    "<h1>&#x1f512; LestraOS Sandbox Manager</h1>"
    "<div id=\"sandboxes\">Loading...</div>"
    "<h2>Create Sandbox</h2>"
    "Name: <input id=\"sname\" value=\"sandbox\"> "
    "Port: <input id=\"sport\" value=\"8081\" size=\"6\"> "
    "<button onclick=\"createSb()\">Create</button>"
    "<div id=\"status\">Ready.</div>"
    "<script>"
    "function api(m,url,b){return fetch(url,{method:m,"
    "headers:{'Content-Type':'application/json'},"
    "body:b?JSON.stringify(b):undefined}).then(r=>r.text());}"
    "function load(){api('GET','/api/sandboxes').then(t=>{"
    "let d=JSON.parse(t);let h='';"
    "d.sandboxes.forEach(s=>{"
    "let cls=s.active?'running':'stopped';"
    "h+='<div class=\"box '+cls+'\">';"
    "h+='<b>['+s.id+'] '+s.name+'</b> — '+"
    "(s.active?'RUNNING (pid '+s.pid+')':'STOPPED')+'<br>';"
    "h+='Port: '+s.port+' | Net: '+(s.network_disabled?'off':'on')+' | '"
    "+'Mem: '+(s.memory_limit/1048576)+'MB<br>';"
    "h+='<button onclick=\"startSb('+s.id+')\">Start</button> ';"
    "h+='<button onclick=\"stopSb('+s.id+')\">Stop</button> ';"
    "h+='<button onclick=\"destroySb('+s.id+')\">Destroy</button><br>';"
    "h+='</div>';});document.getElementById('sandboxes').innerHTML=h;});}"
    "function createSb(){let n=document.getElementById('sname').value;"
    "let p=parseInt(document.getElementById('sport').value)||0;"
    "api('POST','/api/sandbox/create',{name:n,port:p}).then(t=>{"
    "log(t);load();});}"
    "function startSb(id){let c=prompt('Command to run:','echo hello');"
    "if(c)api('POST','/api/sandbox/'+id+'/start',{cmd:c}).then(t=>{"
    "log(t);load();});}"
    "function stopSb(id){api('POST','/api/sandbox/'+id+'/stop').then(t=>{"
    "log(t);load();});}"
    "function destroySb(id){api('DELETE','/api/sandbox/'+id).then(t=>{"
    "log(t);load();});}"
    "function log(m){document.getElementById('status').textContent+=m+'\\n';}"
    "setInterval(load,3000);load();"
    "</script></body></html>";

/* ----- JSON builders --------------------------------------------------- */

static int build_sandboxes_json(char* buf, int bufsize) {
    int off = 0;
    off += ksnprintf(buf + off, bufsize - off, "{\"sandboxes\":[");
    int first = 1;
    for (int i = 1; i <= SANDBOX_MAX; i++) {
        struct sandbox_info info;
        if (sandbox_status(i, &info) != 0) continue;
        if (!first) buf[off++] = ',';
        first = 0;
        off += ksnprintf(buf + off, bufsize - off,
                         "{\"id\":%d,\"pid\":%d,\"name\":\"%s\","
                         "\"active\":%d,\"network_disabled\":%d,"
                         "\"memory_limit\":%lu,\"memory_used\":%lu,"
                         "\"max_open_fds\":%d,\"port\":%d}",
                         info.id, info.pid, info.name,
                         info.active, info.network_disabled,
                         (unsigned long)info.memory_limit,
                         (unsigned long)info.memory_used,
                         info.max_open_fds, info.port);
    }
    off += ksnprintf(buf + off, bufsize - off, "]}");
    return off;
}

/* Minimal JSON string value extraction: finds "key":"value" */
static int json_get_string(const char* json, const char* key,
                           char* out, int outsz) {
    char needle[64];
    ksnprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < outsz - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

static int json_get_int(const char* json, const char* key, int defval) {
    char needle[64];
    ksnprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return defval;
    p += strlen(needle);
    while (*p == ' ') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') return defval;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    return neg ? -v : v;
}

/* ----- Request handler ------------------------------------------------- */

static int handle_request(const char* raw, int raw_len,
                          char* resp_buf, int resp_bufsize) {
    struct http_request req;
    parse_http_request(raw, raw_len, &req);

    /* GET / — serve web UI */
    if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/") == 0) {
        return http_response_build(resp_buf, resp_bufsize,
                                   200, "OK", "text/html",
                                   web_ui_html, (int)strlen(web_ui_html));
    }

    /* GET /api/sandboxes */
    if (strcmp(req.method, "GET") == 0 &&
        strcmp(req.path, "/api/sandboxes") == 0) {
        char json[2048];
        build_sandboxes_json(json, sizeof(json));
        return http_ok(resp_buf, resp_bufsize, json);
    }

    /* POST /api/sandbox/create */
    if (strcmp(req.method, "POST") == 0 &&
        strcmp(req.path, "/api/sandbox/create") == 0) {
        char name[SANDBOX_NAME_LEN] = {0};
        json_get_string(req.body, "name", name, sizeof(name));
        int port = json_get_int(req.body, "port", 0);

        int id = sandbox_create(name[0] ? name : NULL, port);
        if (id < 0) {
            return http_bad_request(resp_buf, resp_bufsize,
                                    "Failed to create sandbox (max reached?)");
        }
        char body[128];
        ksnprintf(body, sizeof(body),
                  "{\"id\":%d,\"message\":\"sandbox created\"}", id);
        return http_created(resp_buf, resp_bufsize, body);
    }

    /* POST /api/sandbox/:id/start */
    if (strcmp(req.method, "POST") == 0 &&
        path_ends_with(req.path, "/start")) {
        int id = extract_sandbox_id(req.path);
        if (id < 0) return http_not_found(resp_buf, resp_bufsize);
        char cmd[SANDBOX_CMD_LEN] = {0};
        json_get_string(req.body, "cmd", cmd, sizeof(cmd));
        if (!cmd[0]) {
            return http_bad_request(resp_buf, resp_bufsize,
                                    "Missing cmd field");
        }
        int rc = sandbox_start(id, cmd);
        if (rc < 0) {
            return http_error(resp_buf, resp_bufsize, 500,
                              "Failed to start sandbox");
        }
        char body[128];
        ksnprintf(body, sizeof(body),
                  "{\"id\":%d,\"message\":\"started\"}", id);
        return http_ok(resp_buf, resp_bufsize, body);
    }

    /* POST /api/sandbox/:id/stop */
    if (strcmp(req.method, "POST") == 0 &&
        path_ends_with(req.path, "/stop")) {
        int id = extract_sandbox_id(req.path);
        if (id < 0) return http_not_found(resp_buf, resp_bufsize);
        sandbox_stop(id);
        char body[128];
        ksnprintf(body, sizeof(body),
                  "{\"id\":%d,\"message\":\"stopped\"}", id);
        return http_ok(resp_buf, resp_bufsize, body);
    }

    /* DELETE /api/sandbox/:id */
    if (strcmp(req.method, "DELETE") == 0) {
        int id = extract_sandbox_id(req.path);
        if (id < 0) return http_not_found(resp_buf, resp_bufsize);
        sandbox_destroy(id);
        char body[128];
        ksnprintf(body, sizeof(body),
                  "{\"id\":%d,\"message\":\"destroyed\"}", id);
        return http_ok(resp_buf, resp_bufsize, body);
    }

    /* GET /api/sandbox/:id/output */
    if (strcmp(req.method, "GET") == 0 &&
        path_ends_with(req.path, "/output")) {
        int id = extract_sandbox_id(req.path);
        if (id < 0) return http_not_found(resp_buf, resp_bufsize);
        if (id < 1 || id > SANDBOX_MAX) {
            return http_bad_request(resp_buf, resp_bufsize, "Invalid id");
        }
        char json[OUTPUT_BUF_SIZE + 128];
        ksnprintf(json, sizeof(json),
                  "{\"id\":%d,\"output\":\"%s\"}",
                  id, output_buf[id - 1]);
        return http_ok(resp_buf, resp_bufsize, json);
    }

    /* POST /api/sandbox/:id/exec */
    if (strcmp(req.method, "POST") == 0 &&
        path_ends_with(req.path, "/exec")) {
        int id = extract_sandbox_id(req.path);
        if (id < 0) return http_not_found(resp_buf, resp_bufsize);
        char cmd[SANDBOX_CMD_LEN] = {0};
        json_get_string(req.body, "cmd", cmd, sizeof(cmd));
        if (!cmd[0]) {
            return http_bad_request(resp_buf, resp_bufsize,
                                    "Missing cmd field");
        }
        int rc = sandbox_start(id, cmd);
        char body[128];
        if (rc < 0) {
            ksnprintf(body, sizeof(body),
                      "{\"id\":%d,\"error\":\"exec failed\"}", id);
            return http_error(resp_buf, resp_bufsize, 500, body);
        }
        ksnprintf(body, sizeof(body),
                  "{\"id\":%d,\"pid\":%d,\"message\":\"executed\"}",
                  id, rc);
        return http_ok(resp_buf, resp_bufsize, body);
    }

    /* 404 fallback */
    return http_not_found(resp_buf, resp_bufsize);
}

/* ----- Public API ------------------------------------------------------ */

void sandbox_server_start(int port) {
    if (server_running) {
        pr_warn("sandbox_server: already running on port %d\n", server_port);
        return;
    }
    server_port = port > 0 ? port : 8080;
    server_running = 1;
    memset(output_buf, 0, sizeof(output_buf));
    memset(output_len, 0, sizeof(output_len));
    pr_info("sandbox_server: started on port %d\n", server_port);
    /* TCP listener setup will be wired when the TCP server API is ready.
     * For now, handle_request() is callable from the TCP accept callback. */
}

void sandbox_server_stop(void) {
    if (!server_running) return;
    server_running = 0;
    server_socket = -1;
    pr_info("sandbox_server: stopped\n");
}

int sandbox_server_is_running(void) {
    return server_running;
}

int sandbox_server_port(void) {
    return server_port;
}
