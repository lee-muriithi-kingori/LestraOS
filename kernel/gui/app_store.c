/*
 * Lestra OS - App Store
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * A small application store UI. It does NOT actually download packages
 * yet (the underlying kpkg/lestra-pkg.c would need to be wired in),
 * but it shows:
 *   - Search bar at the top
 *   - Category sidebar on the left
 *   - App grid on the right (icon, name, author, install state)
 *   - Install / Uninstall / Open buttons on each card
 *
 * Install/uninstall toggles a local installed[] flag so the UI works
 * even without a real package backend.
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

#define AS_W    720
#define AS_H    460
#define AS_TITLE_H 36
#define AS_PAD  8
#define AS_SEARCH_H 32
#define AS_SIDEBAR_W 140

#define AS_CAT_ALL       0
#define AS_CAT_SYSTEM    1
#define AS_CAT_DEV       2
#define AS_CAT_NET       3
#define AS_CAT_MEDIA     4
#define AS_CAT_GAMES     5
#define AS_CAT_COUNT     6

static const char* as_cat_names[AS_CAT_COUNT] = {
    "All", "System", "Dev", "Net", "Media", "Games"
};

#define AS_MAX_APPS 24

struct as_app {
    char name[32];
    char author[24];
    char glyph[4];       /* single-char icon */
    int  category;
    int  installed;
    uint32_t color;
};

struct as_state {
    struct as_app apps[AS_MAX_APPS];
    int n_apps;
    int category;
    int search_focused;
    char search[32];
    int search_len;
};

static struct as_state as_state;
static struct widget   as_widget;

/* ---------- catalog ---------- */
static void as_seed_catalog(struct as_state* st) {
    static const struct as_app seed[] = {
        {"Terminal+", "lestramk",  ">_", AS_CAT_SYSTEM, 1, 0xFF22D3EE},
        {"Files",     "lestramk",  "F",  AS_CAT_SYSTEM, 1, 0xFFFBBF24},
        {"Editor Pro","lestramk",  "E",  AS_CAT_DEV,    1, 0xFF4ADE80},
        {"Browser",   "lestramk",  "B",  AS_CAT_NET,    1, 0xFF60A5FA},
        {"Mail",      "lestramk",  "M",  AS_CAT_NET,    0, 0xFFF87171},
        {"Calendar",  "lestramk",  "C",  AS_CAT_SYSTEM, 1, 0xFFA78BFA},
        {"Photos",    "lestramk",  "P",  AS_CAT_MEDIA,  1, 0xFFEC4899},
        {"Music",     "lestramk",  "m",  AS_CAT_MEDIA,  0, 0xFFFBBF24},
        {"Video",     "lestramk",  "V",  AS_CAT_MEDIA,  0, 0xFFF87171},
        {"AI Lab",    "lestramk",  "AI", AS_CAT_DEV,    1, 0xFF67E8F9},
        {"Weather",   "lestramk",  "W",  AS_CAT_NET,    0, 0xFF60A5FA},
        {"Clock",     "lestramk",  "Cl", AS_CAT_SYSTEM, 0, 0xFFA78BFA},
        {"Snake",     "lestramk",  "S",  AS_CAT_GAMES,  0, 0xFF4ADE80},
        {"Mines",     "lestramk",  "Mn", AS_CAT_GAMES,  0, 0xFFF87171},
        {"Tetris",    "lestramk",  "T",  AS_CAT_GAMES,  0, 0xFF22D3EE},
        {"Calc",      "lestramk",  "+",  AS_CAT_DEV,    0, 0xFFFBBF24},
    };
    int n = (int)(sizeof(seed) / sizeof(seed[0]));
    if (n > AS_MAX_APPS) n = AS_MAX_APPS;
    for (int i = 0; i < n; i++) {
        memcpy(&st->apps[i], &seed[i], sizeof(struct as_app));
    }
    st->n_apps = n;
}

static int as_match_search(struct as_state* st, const struct as_app* a) {
    if (st->search_len == 0) return 1;
    st->search[st->search_len] = '\0';
    /* Case-insensitive substring match on name or author. */
    char name[32];
    for (int i = 0; i < (int)sizeof(name) - 1 && i < (int)strlen(a->name);
         i++) {
        char c = a->name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        name[i] = c;
        name[i + 1] = '\0';
    }
    char needle[32];
    for (int i = 0; i < st->search_len && i < (int)sizeof(needle) - 1; i++) {
        char c = st->search[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        needle[i] = c;
        needle[i + 1] = '\0';
    }
    return strstr(name, needle) != NULL;
}

/* ---------- draw ---------- */
static void as_draw_search(struct widget* w) {
    int sx = w->x + AS_PAD + AS_SIDEBAR_W + AS_PAD;
    int sy = w->y + AS_TITLE_H + AS_PAD;
    int sw = w->w - 2 * AS_PAD - AS_SIDEBAR_W - AS_PAD;
    fb_fill_rect(sx, sy, sw, AS_SEARCH_H, 0xFF1E293B);
    /* Magnifier */
    fb_draw_circle(sx + 12, sy + 12, 5, UI_TEXT_MUTED);
    fb_draw_line(sx + 16, sy + 16, sx + 20, sy + 20, UI_TEXT_MUTED);
    /* Text */
    fb_draw_string(sx + 32, sy + 8,
                   as_state.search_len ? as_state.search : "Search apps...",
                   as_state.search_len ? UI_TEXT_PRIMARY : UI_TEXT_MUTED);
    /* Cursor */
    uint64_t now = timer_get_ms();
    if (as_state.search_focused && (now / 500) % 2 == 0) {
        int cx = sx + 32 + as_state.search_len * 8;
        fb_fill_rect(cx, sy + 6, 2, 16, UI_ACCENT);
    }
}

static void as_draw_sidebar(struct widget* w) {
    int sx = w->x + AS_PAD;
    int sy = w->y + AS_TITLE_H + AS_PAD;
    int sw = AS_SIDEBAR_W;
    int sh = w->h - AS_TITLE_H - 2 * AS_PAD;
    fb_fill_rect(sx, sy, sw, sh, 0xFF0E1422);
    fb_draw_string_small(sx + 8, sy + 6, "Categories", UI_ACCENT_SOFT);
    for (int i = 0; i < AS_CAT_COUNT; i++) {
        int ry = sy + 28 + i * 24;
        if (i == as_state.category) {
            fb_fill_rect(sx, ry - 2, sw, 22, 0xFF06B6D4);
        }
        fb_draw_string(sx + 12, ry + 4, as_cat_names[i], UI_TEXT_PRIMARY);
    }
}

static void as_draw_grid(struct widget* w) {
    int gx = w->x + AS_PAD + AS_SIDEBAR_W + AS_PAD;
    int gy = w->y + AS_TITLE_H + AS_PAD + AS_SEARCH_H + AS_PAD;
    int gw = w->w - 2 * AS_PAD - AS_SIDEBAR_W - AS_PAD;
    int gh = w->h - AS_TITLE_H - 2 * AS_PAD - AS_SEARCH_H - AS_PAD;
    fb_fill_rect(gx, gy, gw, gh, 0xFF0A0C12);

    int cell_w = 200, cell_h = 96;
    int cols = gw / cell_w;
    if (cols < 1) cols = 1;
    int drawn = 0;
    for (int i = 0; i < as_state.n_apps && drawn < cols * (gh / cell_h); i++) {
        struct as_app* a = &as_state.apps[i];
        if (as_state.category != AS_CAT_ALL && a->category != as_state.category)
            continue;
        if (!as_match_search(&as_state, a)) continue;
        int cx = gx + (drawn % cols) * cell_w + 8;
        int cy = gy + (drawn / cols) * cell_h + 8;
        /* Card */
        fb_draw_rounded(cx, cy, cell_w - 16, cell_h - 16, 6,
                        0xFF111827, 0xFF22D3EE);
        /* Icon tile */
        fb_draw_rounded(cx + 8, cy + 8, 48, 48, 8, a->color, a->color);
        fb_draw_string(cx + 12, cy + 22, a->glyph, 0xFF000000);
        /* Name + author */
        fb_draw_string(cx + 64, cy + 8, a->name, UI_TEXT_PRIMARY);
        fb_draw_string_small(cx + 64, cy + 24, a->author, UI_TEXT_MUTED);
        /* Button */
        const char* btn = a->installed ? "Uninstall" : "Install";
        int bw = fb_text_width(btn) + 16;
        fb_draw_rounded(cx + 64, cy + 44, bw, 18, 4,
                        a->installed ? UI_DANGER : UI_ACCENT,
                        a->installed ? UI_DANGER : UI_ACCENT);
        fb_draw_string_small(cx + 72, cy + 48, btn, 0xFF000000);
        drawn++;
    }
}

static void as_draw(struct widget* w) {
    extern void ui_draw_card(int x, int y, int w, int h, int focused);
    ui_draw_card(w->x, w->y, w->w, w->h, w->focused);
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, AS_TITLE_H - 1, 0xE00E1422);
    fb_draw_string(w->x + 12, w->y + 10, "App Store", UI_TEXT_PRIMARY);
    fb_draw_string(w->x + w->w - 20, w->y + 10, "x", UI_TEXT_MUTED);
    as_draw_sidebar(w);
    as_draw_search(w);
    as_draw_grid(w);
}

/* ---------- events ---------- */
static void as_on_event(struct widget* w, struct event* e) {
    if (e->type == EV_MOUSE_DOWN) {
        int mx = e->mouse.x, my = e->mouse.y;
        int cx = w->x + w->w - 24, cy = w->y + 10;
        if (mx >= cx && mx < cx + 16 && my >= cy && my < cy + 16) {
            w->visible = 0;
            return;
        }
        /* Sidebar */
        int sx = w->x + AS_PAD;
        int sy = w->y + AS_TITLE_H + AS_PAD;
        if (mx >= sx && mx < sx + AS_SIDEBAR_W &&
            my >= sy + 24 && my < sy + 24 + AS_CAT_COUNT * 24) {
            int idx = (my - (sy + 24)) / 24;
            if (idx >= 0 && idx < AS_CAT_COUNT) as_state.category = idx;
            return;
        }
        /* Search bar focus toggle */
        int sh_x = w->x + AS_PAD + AS_SIDEBAR_W + AS_PAD;
        int sh_y = w->y + AS_TITLE_H + AS_PAD;
        int sh_w = w->w - 2 * AS_PAD - AS_SIDEBAR_W - AS_PAD;
        if (mx >= sh_x && mx < sh_x + sh_w &&
            my >= sh_y && my < sh_y + AS_SEARCH_H) {
            as_state.search_focused = 1;
            return;
        }
        as_state.search_focused = 0;
        /* App grid card hit test (Install/Uninstall button). */
        int gx = sh_x;
        int gy = sh_y + AS_SEARCH_H + AS_PAD;
        int gw = sh_w;
        int cell_w = 200, cell_h = 96;
        int cols = gw / cell_w;
        if (cols < 1) cols = 1;
        int drawn = 0;
        for (int i = 0; i < as_state.n_apps; i++) {
            struct as_app* a = &as_state.apps[i];
            if (as_state.category != AS_CAT_ALL && a->category != as_state.category)
                continue;
            if (!as_match_search(&as_state, a)) continue;
            int ax = gx + (drawn % cols) * cell_w + 8;
            int ay = gy + (drawn / cols) * cell_h + 8;
            int bw = fb_text_width(a->installed ? "Uninstall" : "Install") + 16;
            if (mx >= ax + 64 && mx < ax + 64 + bw &&
                my >= ay + 44 && my < ay + 62) {
                a->installed = !a->installed;
                pr_info("app_store: %s %s\n",
                        a->installed ? "installed" : "uninstalled", a->name);
                return;
            }
            drawn++;
        }
    } else if (e->type == EV_KEY_DOWN && as_state.search_focused) {
        if (!keyboard_has_key()) return;
        char c = keyboard_getchar();
        if (c == '\b') {
            if (as_state.search_len > 0) as_state.search_len--;
        } else if (c >= 0x20 && c < 0x7F &&
                   as_state.search_len < (int)sizeof(as_state.search) - 1) {
            as_state.search[as_state.search_len++] = c;
        }
    }
}

/* ---------- public ---------- */
struct widget* app_store_create(int x, int y) {
    memset(&as_state, 0, sizeof(as_state));
    as_seed_catalog(&as_state);
    as_state.category = AS_CAT_ALL;
    as_state.search_focused = 0;

    as_widget.x = x;
    as_widget.y = y;
    as_widget.w = AS_W;
    as_widget.h = AS_H;
    as_widget.visible = 1;
    as_widget.focused = 1;
    as_widget.draggable = 1;
    as_widget.resizable = 0;
    as_widget.draw = as_draw;
    as_widget.on_event = as_on_event;
    as_widget.state = &as_state;
    memcpy(as_widget.title, "App Store", 10);
    return &as_widget;
}
