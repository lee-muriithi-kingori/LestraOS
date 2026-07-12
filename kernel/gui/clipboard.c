/*
 * Lestra OS - Clipboard Manager
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * A 10-item ring buffer that holds the most recent clipboard strings.
 * Win+V opens a popup that lists them; click any to paste it back into
 * the active widget (we route through the keyboard ring buffer via
 * keyboard_inject_char()).
 *
 * API:
 *   clipboard_push(text)            - record a copy/cut
 *   clipboard_recent(out, n)        - get the Nth most recent entry
 *   clipboard_popup_toggle()        - Win+V
 *   clipboard_render()              - compositor draws us on top
 *   clipboard_handle_event(e)       - compositor routes events here
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/printk.h>
#include <string.h>

#define CL_MAX_ENTRIES 10
#define CL_ENTRY_MAX   128
#define CL_POPUP_W     320
#define CL_POPUP_H     240

struct cl_entry {
    char text[CL_ENTRY_MAX];
    int  len;
};

struct cl_state {
    struct cl_entry ring[CL_MAX_ENTRIES];
    int head;       /* next write slot */
    int count;      /* number of valid entries (<= CL_MAX_ENTRIES) */
    int popup_open;
};

static struct cl_state cl_state;

/* Weak: keyboard.c may not export this yet. */
void keyboard_inject_char(char c) __attribute__((weak));
void keyboard_inject_char(char c) { (void)c; }

/* ---------- helpers ---------- */
static int cl_idx(struct cl_state* st, int n) {
    /* n=0 is the most recent. */
    if (n >= st->count) return -1;
    int slot = st->head - 1 - n;
    while (slot < 0) slot += CL_MAX_ENTRIES;
    return slot;
}

/* ---------- public API ---------- */
void clipboard_push(const char* text) {
    if (!text) return;
    size_t l = strlen(text);
    if (l >= CL_ENTRY_MAX) l = CL_ENTRY_MAX - 1;
    struct cl_entry* e = &cl_state.ring[cl_state.head];
    memcpy(e->text, text, l);
    e->text[l] = '\0';
    e->len = (int)l;
    cl_state.head = (cl_state.head + 1) % CL_MAX_ENTRIES;
    if (cl_state.count < CL_MAX_ENTRIES) cl_state.count++;
}

int clipboard_recent(char* out, size_t out_sz, int n) {
    int idx = cl_idx(&cl_state, n);
    if (idx < 0) { if (out_sz > 0) out[0] = '\0'; return 0; }
    struct cl_entry* e = &cl_state.ring[idx];
    size_t l = (size_t)e->len;
    if (l >= out_sz) l = out_sz - 1;
    memcpy(out, e->text, l);
    out[l] = '\0';
    return (int)l;
}

void clipboard_popup_toggle(void) {
    cl_state.popup_open = !cl_state.popup_open;
}

/* ---------- render ---------- */
void clipboard_render(void) {
    if (!cl_state.popup_open) return;
    /* Position: top-right, just below the top bar. */
    int px = (int)fb_w - CL_POPUP_W - 24;
    int py = 72;
    fb_draw_rounded(px, py, CL_POPUP_W, CL_POPUP_H, 8,
                    0xE60E1422, UI_ACCENT);
    fb_draw_string(px + 12, py + 8, "Clipboard", UI_TEXT_PRIMARY);
    fb_draw_string(px + CL_POPUP_W - 20, py + 8, "x", UI_TEXT_MUTED);

    /* Entries (most recent first). */
    int ey = py + 32;
    for (int i = 0; i < cl_state.count && i < 8; i++) {
        int idx = cl_idx(&cl_state, i);
        if (idx < 0) break;
        struct cl_entry* e = &cl_state.ring[idx];
        char line[CL_ENTRY_MAX];
        /* Truncate + show with index. */
        int l = 0;
        l += ksnprintf(line + l, sizeof(line) - l, "%d. ", i + 1);
        const char* src = e->text;
        int room = (int)sizeof(line) - l - 4;
        int sl = e->len;
        if (sl > room) sl = room;
        memcpy(line + l, src, sl);
        l += sl;
        if (e->len > room) {
            line[l++] = '.';
            line[l++] = '.';
            line[l++] = '.';
        }
        line[l] = '\0';
        /* Replace newlines with spaces so we don't break the row layout. */
        for (int j = 0; j < l; j++) {
            if (line[j] == '\n' || line[j] == '\r') line[j] = ' ';
        }
        /* Row hover/select background. */
        if (i % 2 == 0) {
            fb_fill_rect(px + 4, ey - 2, CL_POPUP_W - 8, 22, 0xFF111827);
        }
        fb_draw_string_small(px + 12, ey + 2, line, UI_TEXT_PRIMARY);
        ey += 24;
        if (ey > py + CL_POPUP_H - 16) break;
    }

    if (cl_state.count == 0) {
        fb_draw_string(px + 12, py + 40, "(clipboard is empty)",
                       UI_TEXT_MUTED);
    }
    fb_draw_string_small(px + 12, py + CL_POPUP_H - 16,
                         "Win+V to toggle  |  Click to paste",
                         UI_TEXT_FAINT);
}

/* ---------- events ---------- */
static int cl_hit_close(int mx, int my) {
    int px = (int)fb_w - CL_POPUP_W - 24;
    int py = 72;
    return (mx >= px + CL_POPUP_W - 20 && mx < px + CL_POPUP_W - 4 &&
            my >= py + 6 && my < py + 22);
}

int clipboard_handle_event(struct event* e) {
    /* Win+V toggles the popup no matter what. */
    if (e->type == EV_KEY_DOWN &&
        (e->key.mods & MOD_SUPER) &&
        e->key.scancode == 0x2F /* 'V' */) {
        clipboard_popup_toggle();
        return 1;
    }
    if (!cl_state.popup_open) return 0;
    if (e->type == EV_KEY_DOWN && e->key.scancode == KEY_ESC) {
        cl_state.popup_open = 0;
        return 1;
    }
    if (e->type != EV_MOUSE_DOWN) {
        return (e->type == EV_MOUSE_MOVE || e->type == EV_MOUSE_UP) ? 1 : 0;
    }
    int mx = e->mouse.x, my = e->mouse.y;
    if (cl_hit_close(mx, my)) {
        cl_state.popup_open = 0;
        return 1;
    }
    int px = (int)fb_w - CL_POPUP_W - 24;
    int py = 72;
    if (mx < px || mx >= px + CL_POPUP_W) {
        cl_state.popup_open = 0;  /* click outside = dismiss */
        return 1;
    }
    /* Row click? */
    int ey = py + 32;
    for (int i = 0; i < cl_state.count && i < 8; i++) {
        int idx = cl_idx(&cl_state, i);
        if (idx < 0) break;
        if (my >= ey - 2 && my < ey + 20) {
            struct cl_entry* e = &cl_state.ring[idx];
            for (int j = 0; j < e->len; j++) {
                keyboard_inject_char(e->text[j]);
            }
            cl_state.popup_open = 0;
            return 1;
        }
        ey += 24;
        if (ey > py + CL_POPUP_H - 16) break;
    }
    return 1;
}
