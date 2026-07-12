/*
 * Lestra OS - Offline AI Assistant
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * IMPORTANT: This is NOT a neural language model. It is a rule-based
 * pattern-matching assistant that gives canned responses based on
 * keyword detection. A real offline LLM (like SmolLM, TinyLlama, or
 * Phi-2) requires:
 *
 *   1. Model weights stored on disk (500 MB - 2 GB for a 1B param model)
 *   2. Float/SSE support (we disabled SSE in the kernel for stability)
 *   3. A transformer inference engine (matrix multiply, attention, etc.)
 *   4. Tokenizer + vocabulary
 *
 * Each of these is a major sub-project. A real in-kernel LLM would need
 * months of work and is better suited as a userspace program once we have
 * a proper process loader.
 *
 * This offline assistant is useful for:
 *   - Quick system queries ("what's my IP?", "how much memory?")
 *   - Help with shell commands
 *   - Giving the appearance of offline AI when no network/key is configured
 *
 * When a real API key IS configured, ai_chat() uses the cloud provider
 * instead. This offline assistant is the fallback.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/ai.h>
#include <lestra/net.h>
#include <lestra/mm.h>
#include <lestra/timer.h>
#include <string.h>

/* Check if a string contains a keyword (case-insensitive) */
static int contains_kw(const char* text, const char* kw) {
    int tlen = strlen(text);
    int klen = strlen(kw);
    if (klen > tlen) return 0;
    for (int i = 0; i <= tlen - klen; i++) {
        int match = 1;
        for (int j = 0; j < klen; j++) {
            char tc = text[i+j];
            char kc = kw[j];
            /* lowercase compare */
            if (tc >= 'A' && tc <= 'Z') tc += 32;
            if (kc >= 'A' && kc <= 'Z') kc += 32;
            if (tc != kc) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

/* Generate an offline response based on keyword matching.
 * Returns 1 if a response was generated, 0 if no pattern matched. */
int offline_ai_respond(const char* prompt, char* response, size_t response_size) {
    if (!prompt || !response || response_size == 0) return 0;

    int n = 0;
    const char* p;
    #define APPEND(s) do { \
        p = s; \
        while (*p && n < (int)response_size - 1) response[n++] = *p++; \
    } while(0)

    /* System queries */
    if (contains_kw(prompt, "ip address") || contains_kw(prompt, "my ip") ||
        contains_kw(prompt, "what is my ip")) {
        if (net_is_up()) {
            ipv4_addr_t ip = net_get_ip();
            APPEND("Your IP address is ");
            char buf[16];
            ksnprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                      ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3]);
            APPEND(buf);
            APPEND(".\nGateway: ");
            ipv4_addr_t gw = net_get_gateway();
            ksnprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                      gw.bytes[0], gw.bytes[1], gw.bytes[2], gw.bytes[3]);
            APPEND(buf);
            APPEND("\nDNS: ");
            ipv4_addr_t dns = net_get_dns();
            ksnprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                      dns.bytes[0], dns.bytes[1], dns.bytes[2], dns.bytes[3]);
            APPEND(buf);
            APPEND("\n");
        } else {
            APPEND("Network is not up yet. DHCP may still be in progress.\n");
        }
        response[n] = '\0';
        return 1;
    }

    if (contains_kw(prompt, "memory") || contains_kw(prompt, "meminfo") ||
        contains_kw(prompt, "how much ram") || contains_kw(prompt, "free memory")) {
        char buf[64];
        APPEND("Memory status:\n");
        ksnprintf(buf, sizeof(buf), "  Total: %u MB\n",
                  (unsigned)(pmm_get_total() / (1024 * 1024)));
        APPEND(buf);
        ksnprintf(buf, sizeof(buf), "  Used:  %u MB\n",
                  (unsigned)(pmm_get_used() / (1024 * 1024)));
        APPEND(buf);
        ksnprintf(buf, sizeof(buf), "  Free:  %u MB\n",
                  (unsigned)(pmm_get_free() / (1024 * 1024)));
        APPEND(buf);
        ksnprintf(buf, sizeof(buf), "  Heap:  %u KB used\n",
                  (unsigned)(heap_get_used() / 1024));
        APPEND(buf);
        response[n] = '\0';
        return 1;
    }

    if (contains_kw(prompt, "uptime") || contains_kw(prompt, "how long")) {
        char buf[32];
        uint64_t ms = timer_get_ms();
        uint32_t secs = (uint32_t)(ms / 1000);
        APPEND("System uptime: ");
        ksnprintf(buf, sizeof(buf), "%u seconds (%u minutes %u seconds)\n",
                  secs, secs / 60, secs % 60);
        APPEND(buf);
        response[n] = '\0';
        return 1;
    }

    /* Help queries */
    if (contains_kw(prompt, "help") || contains_kw(prompt, "what can you do")) {
        APPEND("I'm an offline assistant (rule-based, not a neural model).\n\n");
        APPEND("I can help with:\n");
        APPEND("  - System info: ask about IP, memory, uptime\n");
        APPEND("  - Shell commands: 'how do I ping', 'how to install packages'\n");
        APPEND("  - OS info: 'who made you', 'what is lestra os'\n\n");
        APPEND("For real AI chat, configure an API key:\n");
        APPEND("  ai setkey glm <your-key>\n");
        APPEND("  ai chat <your question>\n");
        response[n] = '\0';
        return 1;
    }

    if (contains_kw(prompt, "who made you") || contains_kw(prompt, "who created you") ||
        contains_kw(prompt, "who are you")) {
        APPEND("I'm the Lestra OS offline assistant.\n");
        APPEND("Lestra OS was created by Lee Muriihi Kingori (lestramk.org).\n");
        APPEND("I'm a rule-based assistant, not a neural model — for real AI,\n");
        APPEND("configure a cloud API key with 'ai setkey'.\n");
        response[n] = '\0';
        return 1;
    }

    if (contains_kw(prompt, "what is lestra") || contains_kw(prompt, "about lestra os")) {
        APPEND("Lestra OS is a hobbyist x86_64 operating system.\n");
        APPEND("Features:\n");
        APPEND("  - VESA framebuffer GUI (1024x768x32)\n");
        APPEND("  - TCP/IP stack (E1000 + DHCP + HTTP)\n");
        APPEND("  - AI client (GLM 5.2 / Claude / Gemini / OpenAI)\n");
        APPEND("  - Package manager (65 packages)\n");
        APPEND("  - AHCI disk driver + ext2 filesystem\n");
        APPEND("  - Built by Lee Muriihi Kingori\n");
        response[n] = '\0';
        return 1;
    }

    /* Command help */
    if (contains_kw(prompt, "how do i ping") || contains_kw(prompt, "how to ping")) {
        APPEND("To ping a host:\n");
        APPEND("  ping <host-or-ip>\n");
        APPEND("Examples:\n");
        APPEND("  ping 10.0.2.2\n");
        APPEND("  ping example.com\n");
        response[n] = '\0';
        return 1;
    }

    if (contains_kw(prompt, "install") && contains_kw(prompt, "package")) {
        APPEND("To install a package:\n");
        APPEND("  pkg install <name>\n");
        APPEND("Example: pkg install hello\n");
        APPEND("List available packages: pkg list\n");
        response[n] = '\0';
        return 1;
    }

    if (contains_kw(prompt, "hello") || contains_kw(prompt, "hi ") ||
        prompt[0] == 'h' && prompt[1] == 'i' && (prompt[2] == '\0' || prompt[2] == ' ')) {
        APPEND("Hello! I'm the Lestra OS offline assistant.\n");
        APPEND("Type 'help' to see what I can do, or ask about\n");
        APPEND("system info (IP, memory, uptime).\n");
        response[n] = '\0';
        return 1;
    }

    if (contains_kw(prompt, "thank")) {
        APPEND("You're welcome! Anything else I can help with?\n");
        response[n] = '\0';
        return 1;
    }

    /* No pattern matched */
    APPEND("[offline] I don't have a pattern for that prompt.\n");
    APPEND("I'm a rule-based assistant, not a full language model.\n");
    APPEND("Try asking about: IP address, memory, uptime, or type 'help'.\n");
    APPEND("For real AI, configure a cloud key: ai setkey glm <key>\n");
    response[n] = '\0';
    return 1;
}
