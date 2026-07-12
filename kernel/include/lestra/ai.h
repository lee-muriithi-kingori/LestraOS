/*
 * Lestra OS - AI Subsystem header
 * Copyright (c) 2026 lestramk.org
 *
 * Provides:
 *   - Multi-provider API key storage (OpenAI, Anthropic, Gemini, Z.ai GLM)
 *   - Tool-call framework for agentic abilities
 *   - Chat + tool dispatch loop
 *
 * Network requirements:
 *   A real implementation needs a TCP/IP stack + HTTP client + TLS.
 *   When the network stack is not present, calls return a clearly-labeled
 *   simulated response so the UX can be demonstrated.
 */
#ifndef LESTRA_AI_H
#define LESTRA_AI_H

#include <lestra/types.h>

/* Supported providers */
#define AI_PROVIDER_OPENAI    0
#define AI_PROVIDER_ANTHROPIC 1
#define AI_PROVIDER_GEMINI    2
#define AI_PROVIDER_GLM       3  /* Z.ai GLM */
#define AI_PROVIDER_COUNT     4

#define AI_KEY_MAX_LEN   128
#define AI_PROMPT_MAX    1024
#define AI_RESPONSE_MAX  4096
#define AI_TOOL_NAME_MAX 32

/* Tool function signature: takes a JSON-like string of args, returns
 * a string result. The framework owns the result buffer. */
typedef void (*ai_tool_fn)(const char* args, char* out_buf, size_t buf_size);

/* Tool registration entry */
struct ai_tool {
    const char* name;
    const char* description;
    ai_tool_fn handler;
};

/* API key storage */
int  ai_keys_set(int provider, const char* key);
int  ai_keys_get(int provider, char* out, size_t out_size);
int  ai_keys_clear(int provider);
int  ai_keys_count(void);
void ai_keys_list(void);

/* Provider name <-> id conversion */
int         ai_provider_from_name(const char* name);
const char* ai_provider_name(int provider);
const char* ai_provider_endpoint(int provider);
const char* ai_provider_model(int provider);

/* Override a provider's endpoint URL (use http:// for direct/local LLM,
 * or http://localhost:PORT/ for a TLS-terminating proxy in front of a
 * cloud HTTPS API). Returns 0 on success. */
int ai_provider_set_endpoint(int provider, const char* url);

/* Override a provider's model name (e.g. "glm-5.2", "gpt-4-turbo"). */
int ai_provider_set_model(int provider, const char* model);

/* Tool registration */
void        ai_tool_register(const char* name, const char* desc, ai_tool_fn fn);
void        ai_tools_list(void);
const struct ai_tool* ai_tool_find(const char* name);

/* Built-in tools (registered by ai_init) */
void ai_tool_shell(const char* args, char* out, size_t out_size);
void ai_tool_file_read(const char* args, char* out, size_t out_size);
void ai_tool_file_write(const char* args, char* out, size_t out_size);
void ai_tool_pkg_install(const char* args, char* out, size_t out_size);
void ai_tool_pkg_list(const char* args, char* out, size_t out_size);
void ai_tool_meminfo(const char* args, char* out, size_t out_size);
void ai_tool_uptime(const char* args, char* out, size_t out_size);

/* Chat API
 *   prompt: user input
 *   response: buffer for assistant response (AI_RESPONSE_MAX bytes)
 *   returns 0 on success
 *   If no network stack is available, returns a simulated response
 *   with status info. */
int ai_chat(const char* prompt, char* response, size_t response_size);

/* Chat with a specific provider (by ID). Bypasses the auto-pick logic. */
int ai_chat_with_provider(int provider, const char* prompt,
                          char* response, size_t response_size);

/* Chat with automatic tool-calling loop.
 * The AI is given the tool catalog; when it requests a tool, the framework
 * executes the tool and feeds the result back. Loops up to max_iterations. */
int ai_chat_with_tools(const char* prompt, char* response,
                       size_t response_size, int max_iterations);

/* Same but with a specific provider. */
int ai_chat_with_tools_provider(int provider, const char* prompt,
                                char* response, size_t response_size,
                                int max_iterations);

/* Initialize the AI subsystem (registers built-in tools) */
void ai_init(void);

#endif /* LESTRA_AI_H */
