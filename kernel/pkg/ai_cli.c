/*
 * Lestra OS - AI CLI packages (claude, glm, gemini, openai, uai)
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Five shell commands that wrap the existing AI subsystem (kernel/ai/ai.c)
 * with provider-specific defaults and convenient subcommands. These are
 * registered as builtin shell commands — no `pkg install` needed.
 *
 * Commands:
 *   claude chat "prompt"    - chat with Claude (claude-sonnet-4-5)
 *   claude agent "task"     - agentic loop with tools
 *   claude models           - show available models
 *   claude set-model <name> - change model
 *   claude                  - enter REPL
 *
 *   glm "prompt"            - chat with GLM (glm-5.2)
 *   glm -m <model> "prompt" - chat with specific model
 *   glm chat                - REPL
 *   glm tools               - list available tools
 *
 *   gemini "prompt"         - chat with Gemini (gemini-2.5-flash)
 *   gemini chat             - REPL
 *
 *   openai "prompt"         - chat with OpenAI (gpt-4o)
 *   openai chat             - REPL
 *
 *   uai "prompt"            - route by model prefix to the right provider
 *   uai providers           - show routing table
 *
 * NOTE: These commands use the existing ai_chat() function which makes
 * real HTTP POST requests. For cloud APIs (HTTPS), you need a local TLS
 * proxy (see docs/NETWORKING.md). For local LLMs (Ollama), point the
 * endpoint at http://10.0.2.2:11434/v1/chat/completions.
 */

#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/ai.h>
#include <lestra/net.h>
#include <lestra/keyboard.h>
#include <string.h>

/* ----- helpers ----- */

/* Read a line from keyboard into buf (max len). Returns line length. */
static int read_line_kb(char* buf, int maxlen) {
    int len = 0;
    while (1) {
        char c = keyboard_getchar();
        if (c == '\n') {
            printk("\n");
            buf[len] = '\0';
            return len;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                printk("\b \b");
            }
            continue;
        }
        if (len < maxlen - 1 && c >= 0x20 && c < 0x7F) {
            buf[len++] = c;
            printk("%c", c);
        }
    }
}

/* Reconstruct a prompt string from argv[start..argc), space-separated. */
static int build_prompt(int argc, char** argv, int start, char* out, int outsz) {
    int len = 0;
    for (int i = start; i < argc; i++) {
        int wlen = strlen(argv[i]);
        if (len + wlen + 2 >= outsz) break;
        if (len > 0) out[len++] = ' ';
        memcpy(out + len, argv[i], wlen);
        len += wlen;
    }
    out[len] = '\0';
    return len;
}

/* Do a chat with the given provider, print the response. */
static void do_chat(int provider, const char* prompt) {
    char response[AI_RESPONSE_MAX];
    printk("\n[%s] sending prompt (%u chars)...\n",
           ai_provider_name(provider), (unsigned)strlen(prompt));
    int rc = ai_chat_with_provider(provider, prompt, response, sizeof(response));
    printk("\n--- %s Response ---\n%s\n", ai_provider_name(provider), response);
    if (rc != 0) printk("(error code: %d)\n", rc);
}

/* REPL loop for a provider. */
static void do_repl(int provider) {
    char prompt[AI_PROMPT_MAX];
    printk("\n%s REPL (model: %s)\n", ai_provider_name(provider),
           ai_provider_model(provider));
    printk("Type your prompt and press Enter. Empty line to exit.\n\n");
    while (1) {
        printk("%s> ", ai_provider_name(provider));
        int len = read_line_kb(prompt, sizeof(prompt));
        if (len == 0) break;
        do_chat(provider, prompt);
        printk("\n");
    }
    printk("Exiting %s REPL.\n", ai_provider_name(provider));
}

/* ----- claude command ----- */
void cmd_claude(int argc, char** argv) {
    if (argc < 2) {
        do_repl(AI_PROVIDER_ANTHROPIC);
        return;
    }
    if (strcmp(argv[1], "chat") == 0 && argc >= 3) {
        char prompt[AI_PROMPT_MAX];
        build_prompt(argc, argv, 2, prompt, sizeof(prompt));
        do_chat(AI_PROVIDER_ANTHROPIC, prompt);
    } else if (strcmp(argv[1], "agent") == 0 && argc >= 3) {
        char prompt[AI_PROMPT_MAX];
        build_prompt(argc, argv, 2, prompt, sizeof(prompt));
        char response[AI_RESPONSE_MAX];
        printk("\n[claude agent] running agentic loop...\n");
        int rc = ai_chat_with_tools_provider(AI_PROVIDER_ANTHROPIC,
                                             prompt, response, sizeof(response), 8);
        printk("\n--- Agent Output ---\n%s\n", response);
        if (rc != 0) printk("(error code: %d)\n", rc);
    } else if (strcmp(argv[1], "models") == 0) {
        printk("Claude models:\n");
        printk("  claude-sonnet-4-5  (default)\n");
        printk("  claude-opus-4-1\n");
        printk("  claude-3-5-sonnet-20240620\n");
        printk("Use: ai setmodel claude <model>\n");
    } else if (strcmp(argv[1], "set-model") == 0 && argc >= 3) {
        ai_provider_set_model(AI_PROVIDER_ANTHROPIC, argv[2]);
    } else {
        /* Treat all args as a prompt */
        char prompt[AI_PROMPT_MAX];
        build_prompt(argc, argv, 1, prompt, sizeof(prompt));
        if (prompt[0]) {
            do_chat(AI_PROVIDER_ANTHROPIC, prompt);
        } else {
            printk("Usage: claude [chat|agent|models|set-model] [args]\n");
        }
    }
}

/* ----- glm command ----- */
void cmd_glm(int argc, char** argv) {
    if (argc < 2) {
        do_repl(AI_PROVIDER_GLM);
        return;
    }
    /* glm -m <model> "prompt" */
    if (strcmp(argv[1], "-m") == 0 && argc >= 4) {
        ai_provider_set_model(AI_PROVIDER_GLM, argv[2]);
        char prompt[AI_PROMPT_MAX];
        build_prompt(argc, argv, 3, prompt, sizeof(prompt));
        do_chat(AI_PROVIDER_GLM, prompt);
    } else if (strcmp(argv[1], "chat") == 0 && argc >= 3) {
        char prompt[AI_PROMPT_MAX];
        build_prompt(argc, argv, 2, prompt, sizeof(prompt));
        do_chat(AI_PROVIDER_GLM, prompt);
    } else if (strcmp(argv[1], "tools") == 0) {
        ai_tools_list();
    } else if (strcmp(argv[1], "models") == 0) {
        printk("GLM models:\n");
        printk("  glm-5.2  (latest, recommended)\n");
        printk("  glm-4.6\n");
        printk("  glm-4-flash\n");
        printk("Use: ai setmodel glm <model>\n");
    } else {
        char prompt[AI_PROMPT_MAX];
        build_prompt(argc, argv, 1, prompt, sizeof(prompt));
        if (prompt[0]) {
            do_chat(AI_PROVIDER_GLM, prompt);
        } else {
            printk("Usage: glm [\"prompt\" | -m <model> \"prompt\" | chat | tools | models]\n");
        }
    }
}

/* ----- gemini command ----- */
void cmd_gemini(int argc, char** argv) {
    if (argc < 2) {
        do_repl(AI_PROVIDER_GEMINI);
        return;
    }
    if (strcmp(argv[1], "chat") == 0 && argc >= 3) {
        char prompt[AI_PROMPT_MAX];
        build_prompt(argc, argv, 2, prompt, sizeof(prompt));
        do_chat(AI_PROVIDER_GEMINI, prompt);
    } else {
        char prompt[AI_PROMPT_MAX];
        build_prompt(argc, argv, 1, prompt, sizeof(prompt));
        if (prompt[0]) {
            do_chat(AI_PROVIDER_GEMINI, prompt);
        } else {
            printk("Usage: gemini [\"prompt\" | chat]\n");
        }
    }
}

/* ----- openai command ----- */
void cmd_openai(int argc, char** argv) {
    if (argc < 2) {
        do_repl(AI_PROVIDER_OPENAI);
        return;
    }
    if (strcmp(argv[1], "chat") == 0 && argc >= 3) {
        char prompt[AI_PROMPT_MAX];
        build_prompt(argc, argv, 2, prompt, sizeof(prompt));
        do_chat(AI_PROVIDER_OPENAI, prompt);
    } else {
        char prompt[AI_PROMPT_MAX];
        build_prompt(argc, argv, 1, prompt, sizeof(prompt));
        if (prompt[0]) {
            do_chat(AI_PROVIDER_OPENAI, prompt);
        } else {
            printk("Usage: openai [\"prompt\" | chat]\n");
        }
    }
}

/* ----- uai (universal AI) command ----- */
/* Route by model prefix: claude-* to claude, gemini-* to gemini,
 * gpt-XXX or o1-XXX or o3-XXX to openai, glm-* to glm,
 * else cheapest available. */
static int uai_route(void) {
    /* Priority: glm > gemini > claude > openai (cheapest first) */
    extern int ai_keys_count(void);
    if (ai_keys_count() == 0) return -1;

    /* Check which providers have keys set */
    extern int ai_keys_get(int provider, char* out, size_t out_size);
    char key[AI_KEY_MAX_LEN];
    if (ai_keys_get(AI_PROVIDER_GLM, key, sizeof(key)) == 0) return AI_PROVIDER_GLM;
    if (ai_keys_get(AI_PROVIDER_GEMINI, key, sizeof(key)) == 0) return AI_PROVIDER_GEMINI;
    if (ai_keys_get(AI_PROVIDER_ANTHROPIC, key, sizeof(key)) == 0) return AI_PROVIDER_ANTHROPIC;
    if (ai_keys_get(AI_PROVIDER_OPENAI, key, sizeof(key)) == 0) return AI_PROVIDER_OPENAI;
    return -1;
}

void cmd_uai(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: uai [\"prompt\" | providers]\n");
        printk("Routes to the first configured provider (priority: glm > gemini > claude > openai)\n");
        return;
    }
    if (strcmp(argv[1], "providers") == 0) {
        printk("UAI routing priority:\n");
        printk("  1. glm     (Z.ai GLM - cheapest)\n");
        printk("  2. gemini  (Google Gemini)\n");
        printk("  3. claude  (Anthropic Claude)\n");
        printk("  4. openai  (OpenAI GPT)\n");
        printk("\nSet keys with: ai setkey <provider> <key>\n");
        return;
    }
    /* Route to first available provider */
    int provider = uai_route();
    if (provider < 0) {
        printk("uai: no API keys configured. Use 'ai setkey <provider> <key>' first.\n");
        return;
    }
    char prompt[AI_PROMPT_MAX];
    build_prompt(argc, argv, 1, prompt, sizeof(prompt));
    if (prompt[0]) {
        printk("[uai] routing to %s (model: %s)\n",
               ai_provider_name(provider), ai_provider_model(provider));
        do_chat(provider, prompt);
    }
}
