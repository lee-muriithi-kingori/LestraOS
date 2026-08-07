/*
 * Lestra OS - Terminal with tabs
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * A multi-tab terminal card. Up to 8 tabs (TTYTABS_MAX_TABS). Each tab
 * has its own scrollback + input buffer and shares the same shell
 * dispatcher (shell_execute_line from kernel/core/shell.c).
 *
 * Key bindings:
 *   Ctrl+Shift+T  -> new tab
 *   Ctrl+W        -> close current tab
 *   Ctrl+Tab      -> switch to next tab
 *   Ctrl+Shift+Tab -> switch to previous tab
 *
 * Each tab renders an "x" close button on hover. The active tab is
 * highlighted with the cyan accent.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <lestra/printk.h>
#include <string.h>

#define TT_W    720
#define TT_H    420
#define TT_TITLE_H 36
#define TT_TABS_H  28
#define TT_PAD  8
#define CHAR_W  8
#define CHAR_H  16
#define TT_COLS  ((TT_W - 2 * TT_PAD) / CHAR_W)
#define TT_ROWS  ((TT_H - TT_TITLE_H - TT_TABS_H - 2 * TT_PAD) / CHAR_H)
#define TT_MAX_TABS 8

extern void shell_execute_line(const char* line, void (*out)(char c));

struct tt_tab {
    char screen[TT_ROWS][TT_COLS + 1];
    int  cursor_col;
    int  cursor_row;
    char input_buf[256];
    int  input_len;
    char label[12];
    int  used;
};

struct tt_state {
    struct tt_tab tabs[TT_MAX_TABS];
    int active_tab;
    int n_tabs;
};

static struct tt_state tt_state;
static struct widget   tt_widget;

/* ---------- scrollback helpers ---------- */
static void tt_scroll(struct tt_tab* t) {
    for (int r = 0; r < TT_ROWS - 1; r++) {
        memcpy(t->screen[r], t->screen[r + 1], TT_COLS);
        t->screen[r][TT_COLS] = '\0';
    }
    memset(t->screen[TT_ROWS - 1], ' ', TT_COLS);
    t->screen[TT_ROWS - 1][TT_COLS] = '\0';
}

static void tt_putc(struct tt_tab* t, char c) {
    if (c == '\n') {
        t->cursor_col = 0;
        t->cursor_row++;
        if (t->cursor_row >= TT_ROWS) {
            tt_scroll(t);
            t->cursor_row = TT_ROWS - 1;
        }
        return;
    }
    if (c == '\r') { t->cursor_col = 0; return; }
    if (c == '\b') {
        if (t->cursor_col > 0) {
            t->cursor_col--;
            t->screen[t->cursor_row][t->cursor_col] = ' ';
        }
        return;
    }
    if (c == '\t') {
        for (int i = 0; i < 4; i++) tt_putc(t, ' ');
        return;
    }
    if (t->cursor_col >= TT_COLS) {
        t->cursor_col = 0;
        t->cursor_row++;
        if (t->cursor_row >= TT_ROWS) {
            tt_scroll(t);
            t->cursor_row = TT_ROWS - 1;
        }
    }
    t->screen[t->cursor_row][t->cursor_col] = c;
    t->cursor_col++;
}

static void tt_puts(struct tt_tab* t, const char* s) {
    while (*s) tt_putc(t, *s++);
}

static void tt_clear(struct tt_tab* t) {
    for (int r = 0; r < TT_ROWS; r++) {
        memset(t->screen[r], ' ', TT_COLS);
        t->screen[r][TT_COLS] = '\0';
    }
    t->cursor_col = 0;
    t->cursor_row = 0;
}

static void tt_prompt(struct tt_tab* t) {
    tt_puts(t, "lestra:/$ ");
}

static void tt_execute(struct tt_tab* t) {
    t->input_buf[t->input_len] = '\0';
    tt_putc(t, '\n');
    shell_execute_line(t->input_buf, (void (*)(char c))tt_putc);
    /* NOTE: the cast relies on tt_putc being callable as a free function
     * taking a single char. We pass it via a small thunk below. */
    t->input_len = 0;
    tt_prompt(t);
}

/* thunk: shell_execute_line wants void(*)(char), and tt_putc takes a
 * struct tt_tab* + char. We can't bind state in a function pointer in C,
 * so we use a TLS-like "current tab" pointer during execution. */
static struct tt_tab* tt_active_for_output = NULL;
static void tt_output_thunk(char c) {
    if (tt_active_for_output) tt_putc(tt_active_for_output, c);
}

static void tt_run(struct tt_tab* t) {
    t->input_buf[t->input_len] = '\0';
    tt_putc(t, '\n');
    tt_active_for_output = t;
    shell_execute_line(t->input_buf, tt_output_thunk);
    tt_active_for_output = NULL;
    t->input_len = 0;
    tt_prompt(t);
}

/* ---------- tab management ---------- */
static int tt_new_tab(struct tt_state* st) {
    if (st->n_tabs >= TT_MAX_TABS) return -1;
    int idx = -1;
    for (int i = 0; i < TT_MAX_TABS; i++) {
        if (!st->tabs[i].used) { idx = i; break; }
    }
    if (idx < 0) return -1;
    struct tt_tab* t = &st->tabs[idx];
    memset(t, 0, sizeof(*t));
    t->used = 1;
    tt_clear(t);
    ksnprintf(t->label, sizeof(t->label), "Tab %d", idx + 1);
    tt_puts(t, "Lestra Shell (lsh) 1.0\n");
    tt_prompt(t);
    st->n_tabs++;
    st->active_tab = idx;
    return idx;
}

static void tt_close_tab(struct tt_state* st, int idx) {
    if (idx < 0 || idx >= TT_MAX_TABS) return;
    if (!st->tabs[idx].used) return;
    st->tabs[idx].used = 0;
    st->n_tabs--;
    if (st->active_tab == idx) {
        /* Find another tab. */
        int next = -1;
        for (int i = 0; i < TT_MAX_TABS; i++) {
            if (st->tabs[i].used) { next = i; break; }
        }
        st->active_tab = next;
    }
}

static int tt_next_tab(struct tt_state* st) {
    if (st->n_tabs <= 1) return st->active_tab;
    for (int i = st->active_tab + 1; i < st->active_tab + TT_MAX_TABS; i++) {
        int idx = i % TT_MAX_TABS;
        if (st->tabs[idx].used) return idx;
    }
    return st->active_tab;
}

static int tt_prev_tab(struct tt_state* st) {
    if (st->n_tabs <= 1) return st->active_tab;
    for (int i = st->active_tab - 1 + TT_MAX_TABS;
         i > st->active_tab; i--) {
        int idx = i % TT_MAX_TABS;
        if (st->tabs[idx].used) return idx;
    }
    return st->active_tab;
}

/* ---------- draw ---------- */
static void tt_draw_tabs(struct widget* w) {
    int tx = w->x + 2;
    int ty = w->y + TT_TITLE_H;
    int tw = w->w - 4;
    fb_fill_rect(tx, ty, tw, TT_TABS_H, 0xFF0E1422);
    int tab_w = 90;
    int n_drawn = 0;
    for (int i = 0; i < TT_MAX_TABS; i++) {
        if (!tt_state.tabs[i].used) continue;
        int rx = tx + n_drawn * tab_w;
        uint32_t bg = (i == tt_state.active_tab) ? 0xFF0A0C12 : 0xFF1E293B;
        uint32_t bd = (i == tt_state.active_tab) ? UI_ACCENT : 0xFF1E293B;
        fb_fill_rect(rx, ty + 2, tab_w - 4, TT_TABS_H - 2, bg);
        fb_draw_rect(rx, ty + 2, tab_w - 4, TT_TABS_H - 2, bd);
        fb_draw_string_small(rx + 8, ty + 8, tt_state.tabs[i].label,
                             UI_TEXT_PRIMARY);
        /* Close X */
        fb_draw_string_small(rx + tab_w - 18, ty + 8, "x", UI_TEXT_MUTED);
        n_drawn++;
    }
    /* "+" new-tab button at the end */
    int px = tx + n_drawn * tab_w + 4;
    if (tt_state.n_tabs < TT_MAX_TABS) {
        fb_draw_string_small(px + 4, ty + 8, "+ new", UI_ACCENT_SOFT);
    }
}

static void tt_draw_body(struct widget* w) {
    if (tt_state.active_tab < 0) return;
    struct tt_tab* t = &tt_state.tabs[tt_state.active_tab];
    int bx = w->x + TT_PAD;
    int by = w->y + TT_TITLE_H + TT_TABS_H + TT_PAD;
    int bw = w->w - 2 * TT_PAD;
    int bh = w->h - TT_TITLE_H - TT_TABS_H - 2 * TT_PAD;
    fb_fill_rect(bx, by, bw, bh, 0xFF000000);

    for (int r = 0; r < TT_ROWS; r++) {
        for (int c = 0; c < TT_COLS; c++) {
            char ch = t->screen[r][c];
            if (ch && ch != ' ') {
                fb_draw_char(bx + c * CHAR_W, by + r * CHAR_H,
                             ch, UI_TEXT_PRIMARY);
            }
        }
    }
    /* Cursor */
    uint64_t now = timer_get_ms();
    if ((now / 500) % 2 == 0) {
        int cx = bx + t->cursor_col * CHAR_W;
        int cy = by + t->cursor_row * CHAR_H;
        fb_fill_rect(cx, cy, CHAR_W, CHAR_H, UI_ACCENT);
    }
}

static void tt_draw(struct widget* w) {
    extern void ui_draw_card(int x, int y, int w, int h, int focused);
    ui_draw_card(w->x, w->y, w->w, w->h, w->focused);
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, TT_TITLE_H - 1, 0xE00E1422);
    fb_draw_string(w->x + 12, w->y + 10, "Terminal (tabs)", UI_TEXT_PRIMARY);
    fb_draw_string(w->x + w->w - 20, w->y + 10, "x", UI_TEXT_MUTED);
    tt_draw_tabs(w);
    tt_draw_body(w);
}

/* ---------- events ---------- */
static int tt_hit_tab_close(struct widget* w, int mx, int my, int* idx_out) {
    int tx = w->x + 2;
    int ty = w->y + TT_TITLE_H;
    int tab_w = 90;
    int n_drawn = 0;
    for (int i = 0; i < TT_MAX_TABS; i++) {
        if (!tt_state.tabs[i].used) continue;
        int rx = tx + n_drawn * tab_w;
        if (mx >= rx + tab_w - 18 && mx < rx + tab_w - 4 &&
            my >= ty + 8 && my < ty + 22) {
            *idx_out = i;
            return 1;
        }
        n_drawn++;
    }
    return 0;
}

static int tt_hit_tab_select(struct widget* w, int mx, int my, int* idx_out) {
    int tx = w->x + 2;
    int ty = w->y + TT_TITLE_H;
    int tab_w = 90;
    if (my < ty + 2 || my >= ty + TT_TABS_H) return 0;
    int n_drawn = 0;
    for (int i = 0; i < TT_MAX_TABS; i++) {
        if (!tt_state.tabs[i].used) continue;
        int rx = tx + n_drawn * tab_w;
        if (mx >= rx && mx < rx + tab_w - 4) {
            *idx_out = i;
            return 1;
        }
        n_drawn++;
    }
    /* New-tab button? */
    int px = tx + n_drawn * tab_w + 4;
    if (mx >= px && mx < px + 48) {
        *idx_out = -1;  /* sentinel: "new tab" */
        return 1;
    }
    return 0;
}

static void tt_on_event(struct widget* w, struct event* e) {
    if (e->type == EV_MOUSE_DOWN) {
        int mx = e->mouse.x, my = e->mouse.y;
        /* Close button */
        int cx = w->x + w->w - 24, cy = w->y + 10;
        if (mx >= cx && mx < cx + 16 && my >= cy && my < cy + 16) {
            w->visible = 0;
            return;
        }
        /* Tab close */
        int idx = -1;
        if (tt_hit_tab_close(w, mx, my, &idx)) {
            tt_close_tab(&tt_state, idx);
            return;
        }
        /* Tab select / new */
        if (tt_hit_tab_select(w, mx, my, &idx)) {
            if (idx == -1) tt_new_tab(&tt_state);
            else           tt_state.active_tab = idx;
            return;
        }
    } else if (e->type == EV_KEY_DOWN) {
        uint8_t mods = e->key.mods;
        uint8_t sc = e->key.scancode;
        /* Ctrl+Shift+T -> new tab */
        if ((mods & MOD_CTRL) && (mods & MOD_SHIFT) && sc == KEY_T) {
            tt_new_tab(&tt_state);
            return;
        }
        /* Ctrl+W -> close tab */
        if ((mods & MOD_CTRL) && sc == KEY_W) {
            tt_close_tab(&tt_state, tt_state.active_tab);
            return;
        }
        /* Ctrl+Tab -> next, Ctrl+Shift+Tab -> prev */
        if ((mods & MOD_CTRL) && sc == KEY_TAB) {
            if (mods & MOD_SHIFT) {
                tt_state.active_tab = tt_prev_tab(&tt_state);
            } else {
                tt_state.active_tab = tt_next_tab(&tt_state);
            }
            return;
        }
        /* Regular keyboard input goes to the active tab. */
        if (tt_state.active_tab < 0) return;
        if (!keyboard_has_key()) return;
        struct tt_tab* t = &tt_state.tabs[tt_state.active_tab];
        char c = keyboard_getchar();
        if (c == '\n') {
            tt_run(t);
        } else if (c == '\b') {
            if (t->input_len > 0) {
                t->input_len--;
                tt_putc(t, '\b');
            }
        } else if (c >= 0x20 && c < 0x7F) {
            if (t->input_len < (int)sizeof(t->input_buf) - 1) {
                t->input_buf[t->input_len++] = c;
                tt_putc(t, c);
            }
        }
    }
}

/* ---------- public ---------- */
struct widget* terminal_tabs_create(int x, int y) {
    memset(&tt_state, 0, sizeof(tt_state));
    tt_state.active_tab = -1;
    tt_new_tab(&tt_state);

    tt_widget.x = x;
    tt_widget.y = y;
    tt_widget.w = TT_W;
    tt_widget.h = TT_H;
    tt_widget.visible = 1;
    tt_widget.focused = 1;
    tt_widget.draggable = 1;
    tt_widget.resizable = 0;
    tt_widget.draw = tt_draw;
    tt_widget.on_event = tt_on_event;
    tt_widget.state = &tt_state;
    memcpy(tt_widget.title, "Terminal+", 10);
    return &tt_widget;
}
