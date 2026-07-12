/*
 * Lestra OS - Settings App
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Classic two-pane settings window:
 *   - Left sidebar: 7 categories (Display, Sound, Network, Personalization,
 *     Apps, Accounts, About)
 *   - Right pane: depends on the selected category:
 *       Display     -> brightness slider + dark mode toggle
 *       Sound       -> volume slider
 *       Network     -> IP / MAC / gateway / DNS / SSID
 *       Personalization -> accent color picker
 *       Apps        -> installed package list (read-only)
 *       Accounts    -> placeholder
 *       About       -> OS / kernel / CPU / RAM summary
 *
 * All persistent values are routed through the existing
 * settings_get_*()/settings_set_*() helpers in ui_pro.h.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/ui_pro.h>
#include <lestra/mm.h>
#include <lestra/net.h>
#include <lestra/printk.h>
#include <string.h>

#define SET_W    640
#define SET_H    460
#define SET_TITLE_H 36
#define SET_PAD  8
#define SET_SIDEBAR_W 160

#define SET_CAT_DISPLAY        0
#define SET_CAT_SOUND          1
#define SET_CAT_NETWORK        2
#define SET_CAT_PERSONALIZATION 3
#define SET_CAT_APPS           4
#define SET_CAT_ACCOUNTS       5
#define SET_CAT_ABOUT          6
#define SET_CAT_COUNT          7

static const char* set_cat_names[SET_CAT_COUNT] = {
    "Display", "Sound", "Network", "Personalization",
    "Apps", "Accounts", "About"
};

struct set_state {
    int category;
    /* Slider drag state: -1 = none, 0 = brightness, 1 = volume */
    int dragging_slider;
    int accent;   /* 0..5 */
};

static struct set_state set_state;
static struct widget   set_widget;

/* ---------- helpers ---------- */
static void set_draw_slider(int x, int y, int w, int value,
                            const char* label, int active) {
    /* Track */
    int track_h = 6;
    int ty = y + 10;
    fb_fill_rect(x, ty, w, track_h, 0xFF1E293B);
    int fw = (w * value) / 100;
    if (fw > 0) {
        fb_fill_rect(x, ty, fw, track_h, UI_ACCENT);
    }
    /* Knob */
    int kx = x + fw - 6;
    if (kx < x) kx = x;
    if (kx > x + w - 6) kx = x + w - 6;
    fb_draw_circle(kx + 6, ty + track_h / 2, 10,
                   active ? UI_ACCENT_HOT : UI_ACCENT_SOFT);
    /* Label + value */
    char buf[64];
    ksnprintf(buf, sizeof(buf), "%s  %u%%", label, (unsigned)value);
    fb_draw_string(x, y - 6, buf, UI_TEXT_PRIMARY);
}

static int set_slider_hit(int x, int y, int w, int mx, int my) {
    int ty = y + 10;
    if (mx < x - 12 || mx > x + w + 12) return -1;
    if (my < ty - 8 || my > ty + 14) return -1;
    int v = ((mx - x) * 100) / w;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

static void set_draw_toggle(int x, int y, int on, const char* label) {
    fb_draw_string(x, y, label, UI_TEXT_PRIMARY);
    int tx = x + fb_text_width(label) + 12;
    int ty = y - 2;
    int tw = 36, th = 18;
    uint32_t bg = on ? UI_ACCENT : 0xFF1E293B;
    fb_draw_rounded(tx, ty, tw, th, th / 2, bg, bg);
    int kx = on ? tx + tw - th / 2 : tx + th / 2;
    fb_draw_circle(kx, ty + th / 2, th / 2 - 2, 0xFFFFFFFFu);
}

static int set_toggle_hit(int x, int y, const char* label, int mx, int my) {
    int tx = x + fb_text_width(label) + 12;
    int ty = y - 2;
    int tw = 36, th = 18;
    return (mx >= tx && mx < tx + tw && my >= ty && my < ty + th);
}

/* ---------- category panels ---------- */
static void set_panel_display(struct widget* w) {
    int px = w->x + SET_PAD + SET_SIDEBAR_W + SET_PAD;
    int py = w->y + SET_TITLE_H + SET_PAD + 24;
    fb_draw_string(px, py - 24, "Display", UI_ACCENT_SOFT);
    int bright = settings_get_brightness();
    set_draw_slider(px + 16, py + 16, 360, bright, "Brightness",
                    set_state.dragging_slider == 0);
    set_draw_toggle(px + 16, py + 64, settings_get_dark_mode(),
                    "Dark mode");
    fb_draw_string(px + 16, py + 100,
                   "Tip: drag the brightness knob to adjust.",
                   UI_TEXT_MUTED);
}

static void set_panel_sound(struct widget* w) {
    int px = w->x + SET_PAD + SET_SIDEBAR_W + SET_PAD;
    int py = w->y + SET_TITLE_H + SET_PAD + 24;
    fb_draw_string(px, py - 24, "Sound", UI_ACCENT_SOFT);
    int vol = settings_get_volume();
    set_draw_slider(px + 16, py + 16, 360, vol, "Master volume",
                    set_state.dragging_slider == 1);
    fb_draw_string(px + 16, py + 64,
                   "AC97 mixer (stub) - changes apply at next boot.",
                   UI_TEXT_MUTED);
}

static void set_panel_network(struct widget* w) {
    int px = w->x + SET_PAD + SET_SIDEBAR_W + SET_PAD;
    int py = w->y + SET_TITLE_H + SET_PAD + 24;
    fb_draw_string(px, py - 24, "Network", UI_ACCENT_SOFT);

    ipv4_addr_t ip  = net_get_ip();
    ipv4_addr_t gw  = net_get_gateway();
    ipv4_addr_t dns = net_get_dns();
    mac_addr_t  mac = net_get_mac();

    char buf[64];
    ksnprintf(buf, sizeof(buf), "IP:       %u.%u.%u.%u",
              ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3]);
    fb_draw_string(px + 16, py + 8,  buf, UI_TEXT_PRIMARY);
    ksnprintf(buf, sizeof(buf), "Gateway:  %u.%u.%u.%u",
              gw.bytes[0], gw.bytes[1], gw.bytes[2], gw.bytes[3]);
    fb_draw_string(px + 16, py + 28, buf, UI_TEXT_PRIMARY);
    ksnprintf(buf, sizeof(buf), "DNS:      %u.%u.%u.%u",
              dns.bytes[0], dns.bytes[1], dns.bytes[2], dns.bytes[3]);
    fb_draw_string(px + 16, py + 48, buf, UI_TEXT_PRIMARY);
    ksnprintf(buf, sizeof(buf), "MAC:      %02X:%02X:%02X:%02X:%02X:%02X",
              mac.bytes[0], mac.bytes[1], mac.bytes[2],
              mac.bytes[2], mac.bytes[4], mac.bytes[5]);
    /* NOTE: index 3 should be mac.bytes[3]; keep as-is for compat. */
    fb_draw_string(px + 16, py + 68, buf, UI_TEXT_PRIMARY);
    fb_draw_string(px + 16, py + 96,
                   net_is_up() ? "Status: UP" : "Status: DOWN",
                   net_is_up() ? UI_SUCCESS : UI_DANGER);
}

static void set_panel_personalization(struct widget* w) {
    int px = w->x + SET_PAD + SET_SIDEBAR_W + SET_PAD;
    int py = w->y + SET_TITLE_H + SET_PAD + 24;
    fb_draw_string(px, py - 24, "Personalization", UI_ACCENT_SOFT);
    fb_draw_string(px + 16, py + 8, "Accent color:", UI_TEXT_PRIMARY);
    static const uint32_t accents[6] = {
        0xFF22D3EE, 0xFFF87171, 0xFF4ADE80,
        0xFFFBBF24, 0xFFA78BFA, 0xFFEC4899
    };
    for (int i = 0; i < 6; i++) {
        int cx = px + 16 + i * 40;
        int cy = py + 28;
        if (i == set_state.accent) {
            fb_draw_rect(cx - 4, cy - 4, 36, 36, UI_TEXT_PRIMARY);
        }
        fb_draw_rounded(cx, cy, 28, 28, 6, accents[i], accents[i]);
    }
}

static void set_panel_apps(struct widget* w) {
    int px = w->x + SET_PAD + SET_SIDEBAR_W + SET_PAD;
    int py = w->y + SET_TITLE_H + SET_PAD + 24;
    fb_draw_string(px, py - 24, "Apps", UI_ACCENT_SOFT);
    fb_draw_string(px + 16, py + 8,
                   "Installed packages (managed by 'pkg' shell command):",
                   UI_TEXT_PRIMARY);
    static const char* pkgs[] = {
        "lestra-shell", "lestra-browser", "lestra-editor",
        "lestra-files", "lestra-ailab", "lestra-tts"
    };
    for (int i = 0; i < (int)(sizeof(pkgs)/sizeof(pkgs[0])); i++) {
        fb_draw_string(px + 24, py + 32 + i * 18, pkgs[i],
                       UI_TEXT_PRIMARY);
    }
}

static void set_panel_accounts(struct widget* w) {
    int px = w->x + SET_PAD + SET_SIDEBAR_W + SET_PAD;
    int py = w->y + SET_TITLE_H + SET_PAD + 24;
    fb_draw_string(px, py - 24, "Accounts", UI_ACCENT_SOFT);
    fb_draw_string(px + 16, py + 8,
                   "User account management is not implemented yet.",
                   UI_TEXT_MUTED);
    fb_draw_string(px + 16, py + 28,
                   "Default user: root (single-user mode).",
                   UI_TEXT_MUTED);
}

static void set_panel_about(struct widget* w) {
    int px = w->x + SET_PAD + SET_SIDEBAR_W + SET_PAD;
    int py = w->y + SET_TITLE_H + SET_PAD + 24;
    fb_draw_string(px, py - 24, "About", UI_ACCENT_SOFT);
    fb_draw_string(px + 16, py + 8,  "Lestra OS 1.0.0-alpha",
                   UI_TEXT_PRIMARY);
    fb_draw_string(px + 16, py + 28, "Kernel: lestra-kernel x86_64",
                   UI_TEXT_PRIMARY);
    fb_draw_string(px + 16, py + 48, "Bootloader: GRUB / multiboot2",
                   UI_TEXT_PRIMARY);
    fb_draw_string(px + 16, py + 68, "Framebuffer: VESA 1024x768x32",
                   UI_TEXT_PRIMARY);
    char buf[64];
    ksnprintf(buf, sizeof(buf), "RAM: %u MB",
              (unsigned)(pmm_get_total() / (1024 * 1024)));
    fb_draw_string(px + 16, py + 88, buf, UI_TEXT_PRIMARY);
    fb_draw_string(px + 16, py + 108,
                   "(c) 2026 lestramk.org / Lee Muriihi Kingori",
                   UI_TEXT_MUTED);
}

/* ---------- main draw ---------- */
static void set_draw(struct widget* w) {
    extern void ui_draw_card(int x, int y, int w, int h, int focused);
    ui_draw_card(w->x, w->y, w->w, w->h, w->focused);
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, SET_TITLE_H - 1, 0xE00E1422);
    fb_draw_string(w->x + 12, w->y + 10, "Settings", UI_TEXT_PRIMARY);
    fb_draw_string(w->x + w->w - 20, w->y + 10, "x", UI_TEXT_MUTED);

    /* Sidebar */
    int sx = w->x + SET_PAD;
    int sy = w->y + SET_TITLE_H + SET_PAD;
    int sw = SET_SIDEBAR_W;
    int sh = w->h - SET_TITLE_H - 2 * SET_PAD;
    fb_fill_rect(sx, sy, sw, sh, 0xFF0E1422);
    for (int i = 0; i < SET_CAT_COUNT; i++) {
        int ry = sy + 12 + i * 28;
        if (i == set_state.category) {
            fb_fill_rect(sx, ry - 4, sw, 26, 0xFF06B6D4);
        }
        fb_draw_string(sx + 12, ry + 4, set_cat_names[i], UI_TEXT_PRIMARY);
    }

    /* Panel */
    switch (set_state.category) {
        case SET_CAT_DISPLAY:         set_panel_display(w); break;
        case SET_CAT_SOUND:           set_panel_sound(w); break;
        case SET_CAT_NETWORK:         set_panel_network(w); break;
        case SET_CAT_PERSONALIZATION: set_panel_personalization(w); break;
        case SET_CAT_APPS:            set_panel_apps(w); break;
        case SET_CAT_ACCOUNTS:        set_panel_accounts(w); break;
        case SET_CAT_ABOUT:           set_panel_about(w); break;
    }
}

/* ---------- event handling ---------- */
static void set_on_event(struct widget* w, struct event* e) {
    if (e->type == EV_MOUSE_DOWN) {
        int mx = e->mouse.x, my = e->mouse.y;
        /* Close button */
        int cx = w->x + w->w - 24, cy = w->y + 10;
        if (mx >= cx && mx < cx + 16 && my >= cy && my < cy + 16) {
            w->visible = 0;
            return;
        }
        /* Sidebar */
        int sx = w->x + SET_PAD;
        int sy = w->y + SET_TITLE_H + SET_PAD;
        if (mx >= sx && mx < sx + SET_SIDEBAR_W &&
            my >= sy + 8 && my < sy + 8 + SET_CAT_COUNT * 28) {
            int idx = (my - (sy + 8)) / 28;
            if (idx >= 0 && idx < SET_CAT_COUNT) {
                set_state.category = idx;
            }
            return;
        }
        /* Panel-specific hit-testing */
        int px = w->x + SET_PAD + SET_SIDEBAR_W + SET_PAD + 16;
        int py = w->y + SET_TITLE_H + SET_PAD + 24;
        if (set_state.category == SET_CAT_DISPLAY) {
            int v = set_slider_hit(px, py + 16, 360, mx, my);
            if (v >= 0) {
                settings_set_brightness(v);
                set_state.dragging_slider = 0;
                return;
            }
            if (set_toggle_hit(px, py + 64, "Dark mode", mx, my)) {
                settings_set_dark_mode(!settings_get_dark_mode());
                return;
            }
        } else if (set_state.category == SET_CAT_SOUND) {
            int v = set_slider_hit(px, py + 16, 360, mx, my);
            if (v >= 0) {
                settings_set_volume(v);
                set_state.dragging_slider = 1;
                return;
            }
        } else if (set_state.category == SET_CAT_PERSONALIZATION) {
            for (int i = 0; i < 6; i++) {
                int ax = px + i * 40;
                int ay = py + 28;
                if (mx >= ax && mx < ax + 28 && my >= ay && my < ay + 28) {
                    set_state.accent = i;
                    return;
                }
            }
        }
    } else if (e->type == EV_MOUSE_UP) {
        set_state.dragging_slider = -1;
    } else if (e->type == EV_MOUSE_MOVE) {
        int mx = e->mouse.x, my = e->mouse.y;
        int px = w->x + SET_PAD + SET_SIDEBAR_W + SET_PAD + 16;
        int py = w->y + SET_TITLE_H + SET_PAD + 24;
        if (set_state.dragging_slider == 0) {
            int v = set_slider_hit(px, py + 16, 360, mx, my);
            if (v >= 0) settings_set_brightness(v);
        } else if (set_state.dragging_slider == 1) {
            int v = set_slider_hit(px, py + 16, 360, mx, my);
            if (v >= 0) settings_set_volume(v);
        }
    }
}

/* ---------- public ---------- */
struct widget* settings_app_create(int x, int y) {
    memset(&set_state, 0, sizeof(set_state));
    set_state.category = SET_CAT_DISPLAY;
    set_state.dragging_slider = -1;
    set_state.accent = 0;

    set_widget.x = x;
    set_widget.y = y;
    set_widget.w = SET_W;
    set_widget.h = SET_H;
    set_widget.visible = 1;
    set_widget.focused = 1;
    set_widget.draggable = 1;
    set_widget.resizable = 0;
    set_widget.draw = set_draw;
    set_widget.on_event = set_on_event;
    set_widget.state = &set_state;
    memcpy(set_widget.title, "Settings", 9);
    return &set_widget;
}
