/*
 * Lestra OS - GUI AI Lab card
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * A floating card with a chat interface for talking to AI providers.
 * Shows message bubbles (user right-aligned, assistant left-aligned),
 * an input field at the bottom, and a provider indicator in the title bar.
 *
 * Uses the existing AI subsystem (ai_chat_with_provider) to send messages.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/printk.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <lestra/ai.h>
#include <string.h>

#define AILAB_W  640
#define AILAB_H  520
#define AILAB_TITLE_H 36
#define AILAB_PAD 8
#define CHAR_W 8
#define CHAR_H 16
#define AILAB_COLS ((AILAB_W - 2 * AILAB_PAD) / CHAR_W)
#define AILAB_ROWS ((AILAB_H - AILAB_TITLE_H - 2 * AILAB_PAD - 40) / CHAR_H)
#define MAX_BUBBLES 32
#define BUBBLE_TEXT_LEN 256

struct chat_bubble {
    int is_user;  /* 1 = user (right), 0 = assistant (left) */
    char text[BUBBLE_TEXT_LEN];
    int text_len;
};

struct ailab_state {
    struct chat_bubble bubbles[MAX_BUBBLES];
    int n_bubbles;
    char input_buf[256];
    int input_len;
    int active;
    int provider;  /* AI_PROVIDER_GLM etc */
    int sending;   /* 1 while waiting for response */
};

static struct ailab_state ailab_state;
static struct widget ailab_widget;

static void ailab_draw(struct widget* w);
static void ailab_on_event(struct widget* w, struct event* e);

static void ailab_add_bubble(int is_user, const char* text) {
    if (ailab_state.n_bubbles >= MAX_BUBBLES) {
        /* Scroll: drop oldest */
        for (int i = 0; i < MAX_BUBBLES - 1; i++) {
            ailab_state.bubbles[i] = ailab_state.bubbles[i + 1];
        }
        ailab_state.n_bubbles = MAX_BUBBLES - 1;
    }
    struct chat_bubble* b = &ailab_state.bubbles[ailab_state.n_bubbles++];
    b->is_user = is_user;
    b->text_len = strlen(text);
    if (b->text_len >= BUBBLE_TEXT_LEN) b->text_len = BUBBLE_TEXT_LEN - 1;
    memcpy(b->text, text, b->text_len);
    b->text[b->text_len] = '\0';
}

static void ailab_send(void) {
    if (ailab_state.input_len == 0) return;
    ailab_state.input_buf[ailab_state.input_len] = '\0';

    /* Add user bubble */
    ailab_add_bubble(1, ailab_state.input_buf);

    /* Send to AI */
    ailab_state.sending = 1;
    char response[AI_RESPONSE_MAX];
    int rc = ai_chat_with_provider(ailab_state.provider,
                                    ailab_state.input_buf,
                                    response, sizeof(response));
    ailab_state.sending = 0;

    if (rc == 0) {
        ailab_add_bubble(0, response);
    } else {
        ailab_add_bubble(0, response);  /* error message is in response */
    }

    ailab_state.input_len = 0;
}

static void ailab_draw(struct widget* w) {
    struct ailab_state* st = (struct ailab_state*)w->state;

    /* Card body */
    fb_draw_rounded(w->x, w->y, w->w, w->h, 14,
                    UI_CARD_BG, st->active ? UI_ACCENT : UI_CARD_BORDER);

    /* Title bar */
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, AILAB_TITLE_H - 1, 0xE00E1422);

    /* Title: "AI Lab - <provider>" */
    char title[80];
    ksnprintf(title, sizeof(title), "AI Lab - %s", ai_provider_name(st->provider));
    fb_draw_string(w->x + 12, w->y + 10, title, UI_TEXT_PRIMARY);

    /* Key status dot (green = key set, gray = no key) */
    extern int ai_keys_get(int provider, char* out, size_t out_size);
    char key[AI_KEY_MAX_LEN];
    int has_key = (ai_keys_get(st->provider, key, sizeof(key)) == 0);
    int dot_x = w->x + w->w - 24;
    int dot_y = w->y + 16;
    fb_draw_circle(dot_x, dot_y, 5, has_key ? UI_SUCCESS : UI_TEXT_FAINT);

    /* Close button */
    fb_draw_string(w->x + w->w - 12, w->y + 10, "x", UI_TEXT_MUTED);

    /* Chat area */
    int body_x = w->x + AILAB_PAD;
    int body_y = w->y + AILAB_TITLE_H + AILAB_PAD;
    int body_w = w->w - 2 * AILAB_PAD;
    int body_h = w->h - AILAB_TITLE_H - 2 * AILAB_PAD - 40;
    fb_fill_rect(body_x, body_y, body_w, body_h, 0xFF0A0C12);

    /* Draw bubbles */
    int bubble_y = body_y + 4;
    for (int i = 0; i < st->n_bubbles; i++) {
        struct chat_bubble* b = &st->bubbles[i];
        int lines = (b->text_len / (AILAB_COLS - 4)) + 1;
        int bubble_h = lines * CHAR_H + 8;
        if (bubble_y + bubble_h > body_y + body_h) break;

        int max_text_w = (AILAB_COLS - 4) * CHAR_W;
        int bubble_w = max_text_w;
        int bubble_x;
        if (b->is_user) {
            /* Right-aligned */
            bubble_x = body_x + body_w - bubble_w - 4;
            fb_draw_rounded(bubble_x, bubble_y, bubble_w, bubble_h, 6,
                            0x8022D3EE, 0x8022D3EE);
        } else {
            /* Left-aligned */
            bubble_x = body_x + 4;
            fb_draw_rounded(bubble_x, bubble_y, bubble_w, bubble_h, 6,
                            0x80121828, 0x4094A3B8);
        }

        /* Draw text (word-wrap) */
        int tx = bubble_x + 6;
        int ty = bubble_y + 4;
        int col = 0;
        for (int j = 0; j < b->text_len; j++) {
            if (col >= AILAB_COLS - 4) {
                col = 0;
                ty += CHAR_H;
            }
            if (ty >= bubble_y + bubble_h) break;
            fb_draw_char(tx + col * CHAR_W, ty, b->text[j], UI_TEXT_PRIMARY);
            col++;
        }

        bubble_y += bubble_h + 4;
    }

    /* Sending indicator */
    if (st->sending) {
        uint64_t now = timer_get_ms();
        if ((now / 500) % 2 == 0) {
            fb_draw_string(body_x + 4, bubble_y, "  thinking...", UI_ACCENT_SOFT);
        }
    }

    /* Input field at bottom */
    int input_y = w->y + w->h - 36;
    fb_fill_rect(w->x + AILAB_PAD, input_y, w->w - 2 * AILAB_PAD, 28, 0xFF0A0C12);
    fb_draw_rect(w->x + AILAB_PAD, input_y, w->w - 2 * AILAB_PAD, 28, UI_CARD_BORDER);

    /* Draw input text */
    fb_draw_string(w->x + AILAB_PAD + 6, input_y + 6, st->input_buf, UI_TEXT_PRIMARY);

    /* Blinking cursor */
    uint64_t now = timer_get_ms();
    if ((now / 500) % 2 == 0) {
        int cx = w->x + AILAB_PAD + 6 + st->input_len * CHAR_W;
        fb_fill_rect(cx, input_y + 6, CHAR_W, CHAR_H, UI_ACCENT);
    }

    /* "Send" hint */
    fb_draw_string(w->x + w->w - 60, input_y + 6, "[Enter]", UI_TEXT_MUTED);
}

static void ailab_on_event(struct widget* w, struct event* e) {
    struct ailab_state* st = (struct ailab_state*)w->state;

    if (e->type == EV_MOUSE_DOWN) {
        st->active = 1;
        return;
    }

    if (e->type == EV_KEY_DOWN && st->active && !st->sending) {
        char c;
        if (keyboard_has_key()) {
            c = keyboard_getchar();
            if (c == '\n') {
                ailab_send();
            } else if (c == '\b') {
                if (st->input_len > 0) st->input_len--;
            } else if (c >= 0x20 && c < 0x7F) {
                if (st->input_len < (int)sizeof(st->input_buf) - 1) {
                    st->input_buf[st->input_len++] = c;
                }
            }
        }
    }
}

struct widget* ailab_create(int x, int y) {
    ailab_state.n_bubbles = 0;
    ailab_state.input_len = 0;
    ailab_state.active = 0;
    ailab_state.provider = AI_PROVIDER_GLM;
    ailab_state.sending = 0;

    ailab_add_bubble(0, "Welcome to AI Lab! Type a message and press Enter.");
    ailab_add_bubble(0, "Set your API key first: ai setkey glm <your-key>");

    ailab_widget.x = x;
    ailab_widget.y = y;
    ailab_widget.w = AILAB_W;
    ailab_widget.h = AILAB_H;
    ailab_widget.visible = 1;
    ailab_widget.focused = 0;
    ailab_widget.draggable = 1;
    ailab_widget.resizable = 0;
    ailab_widget.draw = ailab_draw;
    ailab_widget.on_event = ailab_on_event;
    ailab_widget.state = &ailab_state;
    memcpy(ailab_widget.title, "AI Lab", 7);

    return &ailab_widget;
}
