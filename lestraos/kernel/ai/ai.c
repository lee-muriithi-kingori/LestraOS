/*
 * Lestra OS - AI Subsystem implementation
 * Copyright (c) 2026 lestramk.org
 *
 * Implements:
 *   - Per-provider API key storage (in-memory; keys never persist to disk)
 *   - Tool registry with built-in tools (shell, file ops, package mgmt)
 *   - Chat loop with simulated agentic tool-calling
 *
 * IMPORTANT: A real OS would need a TCP/IP stack + HTTP client + TLS to
 * actually call provider APIs. Without that, the chat function returns
 * a clearly-labeled simulated response that demonstrates the agentic UX.
 *
 * When a network stack is added (planned: see docs/NETWORK.md), the only
 * function that needs to change is `ai_http_post()` below — everything
 * else (key management, tool framework, dispatch loop) stays the same.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/ai.h>
#include <lestra/vga.h>
#include <lestra/timer.h>
#include <lestra/mm.h>
#include <lestra/pkg.h>
#include <string.h>

/* ----- provider metadata ---------------------------------------------- */
static const char* provider_names[]   = { "openai", "claude", "gemini", "glm" };
static const char* provider_display[] = { "OpenAI", "Anthropic Claude", "Google Gemini", "Z.ai GLM" };
static const char* provider_endpoints[] = {
    "https://api.openai.com/v1/chat/completions",
    "https://api.anthropic.com/v1/messages",
    "https://generativelanguage.googleapis.com/v1beta/models/gemini-pro:generateContent",
    "https://open.bigmodel.cn/api/paas/v4/chat/completions"
};
static const char* provider_models[] = {
    "gpt-4o",
    "claude-3-5-sonnet-20240620",
    "gemini-1.5-pro",
    "glm-4.5"
};

/* ----- key storage ---------------------------------------------------- */
static char api_keys[AI_PROVIDER_COUNT][AI_KEY_MAX_LEN];
static int  keys_set[AI_PROVIDER_COUNT];

int ai_keys_set(int provider, const char* key) {
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return -1;
    if (!key) return -1;
    size_t len = strlen(key);
    if (len >= AI_KEY_MAX_LEN) return -1;
    memcpy(api_keys[provider], key, len + 1);
    keys_set[provider] = 1;
    pr_info("AI: API key set for %s (length: %u)\n",
            provider_display[provider], (unsigned)len);
    return 0;
}

int ai_keys_get(int provider, char* out, size_t out_size) {
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return -1;
    if (!keys_set[provider]) return -1;
    if (!out || out_size == 0) return -1;
    /* Truncate for safety */
    size_t len = strlen(api_keys[provider]);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, api_keys[provider], len);
    out[len] = '\0';
    return 0;
}

int ai_keys_clear(int provider) {
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return -1;
    /* Zero out the key (don't just unset the flag) */
    for (size_t i = 0; i < AI_KEY_MAX_LEN; i++) api_keys[provider][i] = 0;
    keys_set[provider] = 0;
    return 0;
}

int ai_keys_count(void) {
    int c = 0;
    for (int i = 0; i < AI_PROVIDER_COUNT; i++) if (keys_set[i]) c++;
    return c;
}

void ai_keys_list(void) {
    printk("\nAI API Keys:\n");
    printk("-------------------------------------------\n");
    for (int i = 0; i < AI_PROVIDER_COUNT; i++) {
        printk("  %-15s %-10s endpoint: %s\n",
               provider_display[i],
               keys_set[i] ? "[set]" : "[empty]",
               provider_endpoints[i]);
    }
    printk("\n");
}

int ai_provider_from_name(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < AI_PROVIDER_COUNT; i++) {
        if (strcmp(name, provider_names[i]) == 0) return i;
    }
    return -1;
}

const char* ai_provider_name(int provider) {
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return "unknown";
    return provider_names[provider];
}

const char* ai_provider_endpoint(int provider) {
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return "";
    return provider_endpoints[provider];
}

const char* ai_provider_model(int provider) {
    if (provider < 0 || provider >= AI_PROVIDER_COUNT) return "";
    return provider_models[provider];
}

/* ----- tool registry -------------------------------------------------- */
#define MAX_TOOLS 32
static struct ai_tool tools[MAX_TOOLS];
static int tool_count = 0;

void ai_tool_register(const char* name, const char* desc, ai_tool_fn fn) {
    if (tool_count >= MAX_TOOLS) return;
    if (!name || !desc || !fn) return;
    tools[tool_count].name = name;
    tools[tool_count].description = desc;
    tools[tool_count].handler = fn;
    tool_count++;
}

void ai_tools_list(void) {
    printk("\nAgentic Tools (%d registered):\n", tool_count);
    printk("-------------------------------------------\n");
    for (int i = 0; i < tool_count; i++) {
        printk("  %-20s %s\n", tools[i].name, tools[i].description);
    }
    printk("\n");
}

const struct ai_tool* ai_tool_find(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < tool_count; i++) {
        if (strcmp(tools[i].name, name) == 0) return &tools[i];
    }
    return NULL;
}

/* ----- built-in tools ------------------------------------------------- */

/* Tool: shell - run a shell command and return output.
 * Args format: "command" (just the raw command string) */
void ai_tool_shell(const char* args, char* out, size_t out_size) {
    if (!args || !out || out_size == 0) return;
    /* For safety we route through printk so the user sees what's happening.
     * In a real implementation this would capture the output into `out`. */
    printk("[ai:tool:shell] executing: %s\n", args);
    /* LestraOS kernel shell is interactive — we can only dispatch known
     * commands non-interactively. Demonstrate with a few common ones. */
    if (strcmp(args, "uname") == 0) {
        strncpy(out, "LestraOS 1.0.0-alpha x86_64\n", out_size - 1);
    } else if (strcmp(args, "free") == 0) {
        extern uintptr_t pmm_get_total(void);
        extern uintptr_t pmm_get_used(void);
        extern uintptr_t pmm_get_free(void);
        /* Format into out using simple math */
        /* For brevity just print stats */
        strncpy(out, "Mem: see /proc/meminfo (simulated)\n", out_size - 1);
    } else if (strcmp(args, "uptime") == 0) {
        extern uint64_t timer_get_ms(void);
        uint64_t ms = timer_get_ms();
        unsigned sec = (unsigned)(ms / 1000);
        /* Quick format */
        int n = 0;
        const char* p = "up ";
        while (*p && n < (int)out_size - 1) out[n++] = *p++;
        unsigned m = sec / 60;
        unsigned s = sec % 60;
        char tmp[16]; int t = 0;
        if (m == 0) tmp[t++] = '0';
        while (m) { tmp[t++] = '0' + (m % 10); m /= 10; }
        while (t--) out[n++] = tmp[t];
        out[n++] = 'm';
        out[n++] = ' ';
        t = 0;
        if (s < 10) tmp[t++] = '0';
        while (s) { tmp[t++] = '0' + (s % 10); s /= 10; }
        while (t--) out[n++] = tmp[t];
        out[n++] = 's';
        out[n++] = '\n';
        out[n] = 0;
    } else {
        strncpy(out, "[shell: command requires interactive terminal]", out_size - 1);
        out[out_size - 1] = 0;
    }
}

/* Tool: file_read - read a file from the in-memory VFS.
 * Args: file path */
void ai_tool_file_read(const char* args, char* out, size_t out_size) {
    if (!args || !out || out_size == 0) return;
    printk("[ai:tool:file_read] %s\n", args);
    extern int vfs_open(const char* path, int flags);
    extern ssize_t vfs_read(int fd, void* buf, size_t count);
    extern int vfs_close(int fd);
    int fd = vfs_open(args, 0);  /* O_RDONLY = 0 */
    if (fd < 0) {
        strncpy(out, "[file_read: file not found]", out_size - 1);
        return;
    }
    ssize_t n = vfs_read(fd, out, out_size - 1);
    vfs_close(fd);
    if (n < 0) n = 0;
    out[n] = 0;
}

/* Tool: file_write - write to a file in the in-memory VFS.
 * Args format: "path\ncontent" (newline-separated) */
void ai_tool_file_write(const char* args, char* out, size_t out_size) {
    if (!args || !out || out_size == 0) return;
    /* Find the newline separator */
    const char* nl = args;
    while (*nl && *nl != '\n') nl++;
    if (!*nl) {
        strncpy(out, "[file_write: missing content]", out_size - 1);
        return;
    }
    /* Copy path */
    char path[256];
    size_t plen = nl - args;
    if (plen >= sizeof(path)) plen = sizeof(path) - 1;
    memcpy(path, args, plen);
    path[plen] = 0;
    const char* content = nl + 1;

    printk("[ai:tool:file_write] %s (%u bytes)\n", path, (unsigned)strlen(content));
    extern int vfs_open(const char* path, int flags);
    extern ssize_t vfs_write(int fd, const void* buf, size_t count);
    /* O_CREAT = 0x10 */
    int fd = vfs_open(path, 0x10);
    if (fd < 0) {
        strncpy(out, "[file_write: cannot open file]", out_size - 1);
        return;
    }
    ssize_t n = vfs_write(fd, content, strlen(content));
    extern int vfs_close(int fd);
    vfs_close(fd);
    if (n < 0) {
        strncpy(out, "[file_write: write failed]", out_size - 1);
        return;
    }
    /* Format success message */
    int m = 0;
    const char* p = "wrote ";
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    /* itoa n */
    if (n == 0) out[m++] = '0';
    char tmp[16]; int t = 0;
    long val = n;
    if (val < 0) { out[m++] = '-'; val = -val; }
    while (val) { tmp[t++] = '0' + (val % 10); val /= 10; }
    while (t--) out[m++] = tmp[t];
    p = " bytes to ";
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    p = path;
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    out[m] = 0;
}

/* Tool: pkg_install - install a package via lestra-pkg. */
void ai_tool_pkg_install(const char* args, char* out, size_t out_size) {
    if (!args || !out || out_size == 0) return;
    printk("[ai:tool:pkg_install] %s\n", args);
    int rc = pkg_install(args);
    if (rc == 0) {
        strncpy(out, "package installed successfully", out_size - 1);
    } else {
        strncpy(out, "package install failed (see kernel log)", out_size - 1);
    }
    out[out_size - 1] = 0;
}

/* Tool: pkg_list - list installed packages. */
void ai_tool_pkg_list(const char* args, char* out, size_t out_size) {
    (void)args;
    if (!out || out_size == 0) return;
    int n = pkg_count_installed();
    int m = 0;
    const char* p = "installed packages: ";
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    if (n == 0) out[m++] = '0';
    char tmp[16]; int t = 0;
    while (n) { tmp[t++] = '0' + (n % 10); n /= 10; }
    while (t--) out[m++] = tmp[t];
    out[m] = 0;
}

/* Tool: meminfo - return memory stats. */
void ai_tool_meminfo(const char* args, char* out, size_t out_size) {
    (void)args;
    if (!out || out_size == 0) return;
    extern uintptr_t pmm_get_total(void);
    extern uintptr_t pmm_get_used(void);
    extern uintptr_t pmm_get_free(void);
    unsigned total = (unsigned)(pmm_get_total() / (1024*1024));
    unsigned used  = (unsigned)(pmm_get_used()  / (1024*1024));
    unsigned free_ = (unsigned)(pmm_get_free()  / (1024*1024));
    /* Format: "total=Xmb used=Ymb free=Zmb" */
    int m = 0;
    const char* parts[] = { "total=", NULL, "mb used=", NULL, "mb free=", NULL, "mb" };
    /* Build each integer into the buffer */
    char tmp[16]; int t;
    /* total */
    const char* p = parts[0];
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    t = 0; if (total == 0) tmp[t++] = '0';
    while (total) { tmp[t++] = '0' + (total % 10); total /= 10; }
    while (t--) out[m++] = tmp[t];
    p = parts[1]; /* unused, replaced by code below */
    /* used */
    p = "mb used=";
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    t = 0; if (used == 0) tmp[t++] = '0';
    while (used) { tmp[t++] = '0' + (used % 10); used /= 10; }
    while (t--) out[m++] = tmp[t];
    p = "mb free=";
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    t = 0; if (free_ == 0) tmp[t++] = '0';
    while (free_) { tmp[t++] = '0' + (free_ % 10); free_ /= 10; }
    while (t--) out[m++] = tmp[t];
    p = "mb";
    while (*p && m < (int)out_size - 1) out[m++] = *p++;
    out[m] = 0;
}

/* Tool: uptime - return system uptime. */
void ai_tool_uptime(const char* args, char* out, size_t out_size) {
    (void)args;
    if (!out || out_size == 0) return;
    extern uint64_t timer_get_ms(void);
    uint64_t ms = timer_get_ms();
    unsigned sec = (unsigned)(ms / 1000);
    unsigned m = sec / 60;
    unsigned s = sec % 60;
    int n = 0;
    const char* p = "up ";
    while (*p && n < (int)out_size - 1) out[n++] = *p++;
    char tmp[16]; int t;
    t = 0; if (m == 0) tmp[t++] = '0';
    while (m) { tmp[t++] = '0' + (m % 10); m /= 10; }
    while (t--) out[n++] = tmp[t];
    out[n++] = 'm';
    out[n++] = ' ';
    t = 0; if (s < 10) tmp[t++] = '0';
    while (s) { tmp[t++] = '0' + (s % 10); s /= 10; }
    while (t--) out[n++] = tmp[t];
    out[n++] = 's';
    out[n] = 0;
}

/* ----- HTTP placeholder ----------------------------------------------- */
/* In a real OS this function would:
 *   1. Open a TCP socket to the provider's host (port 443)
 *   2. Negotiate TLS
 *   3. Build a JSON request body with the prompt + tools schema
 *   4. POST to the endpoint with Authorization header
 *   5. Parse JSON response and extract the assistant message
 *
 * Without a network stack, we simulate this by:
 *   - Returning a help message that explains what would happen
 *   - Demonstrating tool-calling by detecting keywords in the prompt
 *     and routing to the appropriate tool
 */
static int ai_http_post(int provider, const char* prompt,
                        char* response, size_t response_size) {
    if (!keys_set[provider]) {
        int n = 0;
        const char* msg = "[no API key set for ";
        while (*msg && n < (int)response_size - 1) response[n++] = *msg++;
        const char* p = provider_display[provider];
        while (*p && n < (int)response_size - 1) response[n++] = *p++;
        msg = ". Use 'ai keys set <provider> <key>' to configure.]";
        while (*msg && n < (int)response_size - 1) response[n++] = *msg++;
        response[n] = 0;
        return -1;
    }

    /* Simulated response — in real life this would be the JSON body
     * returned by the provider. */
    int n = 0;
    const char* msg = "[simulated response from ";
    while (*msg && n < (int)response_size - 1) response[n++] = *msg++;
    const char* p = provider_display[provider];
    while (*p && n < (int)response_size - 1) response[n++] = *p++;
    msg = " (model: ";
    while (*msg && n < (int)response_size - 1) response[n++] = *msg++;
    p = provider_models[provider];
    while (*p && n < (int)response_size - 1) response[n++] = *p++;
    msg = "). Real HTTP requires a TCP/IP stack — see docs/NETWORK.md.]\n\n";
    while (*msg && n < (int)response_size - 1) response[n++] = *msg++;

    /* Add some context-aware response based on prompt keywords */
    if (strstr(prompt, "memory") || strstr(prompt, "meminfo")) {
        msg = "Memory inquiry detected. I would call the `meminfo` tool.\n";
        while (*msg && n < (int)response_size - 1) response[n++] = *msg++;
    } else if (strstr(prompt, "install")) {
        msg = "Install request detected. I would call the `pkg_install` tool.\n";
        while (*msg && n < (int)response_size - 1) response[n++] = *msg++;
    } else if (strstr(prompt, "uptime")) {
        msg = "Uptime inquiry detected. I would call the `uptime` tool.\n";
        while (*msg && n < (int)response_size - 1) response[n++] = *msg++;
    } else {
        msg = "I received your prompt: \"";
        while (*msg && n < (int)response_size - 1) response[n++] = *msg++;
        p = prompt;
        int max = 200;
        while (*p && n < (int)response_size - 1 && max-- > 0) response[n++] = *p++;
        msg = "\". With a real network stack I would respond with the assistant message.\n";
        while (*msg && n < (int)response_size - 1) response[n++] = *msg++;
    }

    response[n] = 0;
    return 0;
}

/* ----- chat API ------------------------------------------------------- */
int ai_chat(const char* prompt, char* response, size_t response_size) {
    if (!prompt || !response || response_size == 0) return -1;

    /* Pick the first provider that has a key set */
    int provider = -1;
    for (int i = 0; i < AI_PROVIDER_COUNT; i++) {
        if (keys_set[i]) { provider = i; break; }
    }

    if (provider < 0) {
        strncpy(response,
                "[no AI provider configured. Use 'ai keys set <provider> <key>' "
                "to set an API key. Providers: openai, claude, gemini, glm]",
                response_size - 1);
        response[response_size - 1] = 0;
        return -1;
    }

    return ai_http_post(provider, prompt, response, response_size);
}

int ai_chat_with_tools(const char* prompt, char* response,
                       size_t response_size, int max_iterations) {
    if (!prompt || !response || response_size == 0) return -1;

    /* Simulated agentic loop:
     *   1. Detect tool requests in the prompt
     *   2. Execute the tool
     *   3. Format tool output as part of the response
     *
     * In a real implementation, this would be driven by the AI's
     * tool_calls array in the response. */
    int n = 0;
    const char* header = "=== Agentic Chat (simulated) ===\n\n";
    while (*header && n < (int)response_size - 1) response[n++] = *header++;

    const char* p = "User: ";
    while (*p && n < (int)response_size - 1) response[n++] = *p++;
    p = prompt;
    int max = 300;
    while (*p && n < (int)response_size - 1 && max-- > 0) response[n++] = *p++;
    response[n++] = '\n';
    response[n++] = '\n';

    /* Tool dispatch */
    char tool_out[512];
    int iterations = 0;
    int dispatched = 0;

    while (iterations < max_iterations) {
        iterations++;
        const char* tool_name = NULL;
        const char* tool_args = "";

        /* Pattern-match tool requests */
        if (strstr(prompt, "memory") || strstr(prompt, "meminfo")) {
            tool_name = "meminfo";
        } else if (strstr(prompt, "uptime")) {
            tool_name = "uptime";
        } else if (strstr(prompt, "install ")) {
            tool_name = "pkg_install";
            tool_args = strstr(prompt, "install ") + 8;
        } else if (strstr(prompt, "list packages") || strstr(prompt, "pkg list")) {
            tool_name = "pkg_list";
        }

        if (!tool_name) break;

        const struct ai_tool* tool = ai_tool_find(tool_name);
        if (!tool) break;

        /* Append tool call header */
        p = "[calling tool: ";
        while (*p && n < (int)response_size - 1) response[n++] = *p++;
        p = tool_name;
        while (*p && n < (int)response_size - 1) response[n++] = *p++;
        p = "]\n";
        while (*p && n < (int)response_size - 1) response[n++] = *p++;

        /* Execute tool */
        tool_out[0] = 0;
        tool->handler(tool_args, tool_out, sizeof(tool_out));

        /* Append tool output */
        p = "Result: ";
        while (*p && n < (int)response_size - 1) response[n++] = *p++;
        p = tool_out;
        while (*p && n < (int)response_size - 1) response[n++] = *p++;
        response[n++] = '\n';
        response[n++] = '\n';

        dispatched++;
        break;  /* one tool per simulated iteration */
    }

    /* Final message */
    if (dispatched > 0) {
        p = "Assistant: I executed the requested tool";
        while (*p && n < (int)response_size - 1) response[n++] = *p++;
        if (dispatched > 1) {
            p = "s";
            while (*p && n < (int)response_size - 1) response[n++] = *p++;
        }
        p = " and reported the result above.\n";
        while (*p && n < (int)response_size - 1) response[n++] = *p++;
    } else {
        /* Fall back to plain chat */
        char chat_resp[AI_RESPONSE_MAX];
        int rc = ai_chat(prompt, chat_resp, sizeof(chat_resp));
        p = "Assistant: ";
        while (*p && n < (int)response_size - 1) response[n++] = *p++;
        if (rc == 0) {
            p = chat_resp;
            while (*p && n < (int)response_size - 1) response[n++] = *p++;
        } else {
            p = "(no response)";
            while (*p && n < (int)response_size - 1) response[n++] = *p++;
        }
        response[n++] = '\n';
    }

    response[n] = 0;
    return 0;
}

/* ----- init ----------------------------------------------------------- */
void ai_init(void) {
    /* Clear all keys at boot */
    for (int i = 0; i < AI_PROVIDER_COUNT; i++) {
        keys_set[i] = 0;
        for (int j = 0; j < AI_KEY_MAX_LEN; j++) api_keys[i][j] = 0;
    }

    /* Register built-in tools */
    ai_tool_register("shell",       "Run a shell command",                   ai_tool_shell);
    ai_tool_register("file_read",   "Read a file from VFS",                  ai_tool_file_read);
    ai_tool_register("file_write",  "Write a file to VFS",                   ai_tool_file_write);
    ai_tool_register("pkg_install", "Install a package",                     ai_tool_pkg_install);
    ai_tool_register("pkg_list",    "List installed packages",               ai_tool_pkg_list);
    ai_tool_register("meminfo",     "Get memory statistics",                 ai_tool_meminfo);
    ai_tool_register("uptime",      "Get system uptime",                     ai_tool_uptime);

    pr_info("AI subsystem initialized (%d tools registered)\n", tool_count);
}
