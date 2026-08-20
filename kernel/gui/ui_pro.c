/*
 * Lestra OS - Professional UI Redesign
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * This file contains the redesigned UI components with a professional
 * dark theme inspired by modern desktop environments.
 *
 * Design language:
 *   - Deep dark background with subtle gradient
 *   - Glassmorphism cards (translucent + blur effect simulation)
 *   - Soft shadows under cards
 *   - Smooth rounded corners (16px radius)
 *   - Cyan accent (#22D3EE) for interactive elements
 *   - Muted text colors for hierarchy
 *   - Status bar with battery, temp, network, time
 *   - Dock with magnification effect on hover
 *   - Desktop icons with hover glow
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/icons.h>
#include <lestra/font.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <lestra/net.h>
#include <lestra/mm.h>
#include <lestra/rtc.h>
#include <lestra/power.h>
#include <lestra/wifi.h>
#include <lestra/assets/wallpaper.h>
#include <lestra/assets/icon_terminal.h>
#include <lestra/assets/icon_ailab.h>
#include <lestra/assets/icon_editor.h>
#include <lestra/assets/icon_media.h>
#include <lestra/assets/icon_files.h>
#include <lestra/assets/icon_settings.h>
#include <lestra/assets/icon_help.h>
#include <lestra/assets/icon_about.h>
#include <lestra/vfs.h>
#include <string.h>

/* Forward declaration */
const uint8_t* get_icon_data(int idx);

/* ===== ENHANCED COLOR PALETTE — Cyan Dark ===== */
#define UI_GLASS_BG      0xE6121620
#define UI_GLASS_BORDER  0x4022D3EE
#define UI_SHADOW        0x40000000
#define UI_HOVER_GLOW    0x3022D3EE
#define UI_ACTIVE_GLOW   0x5022D3EE
#define UI_DOCK_BG       0xCC0E1422
#define UI_DOCK_BORDER   0x3022D3EE

/* ===== SHADOW RENDERING ===== */
/* Draw a soft drop shadow under a rectangular area */
static void draw_shadow(int x, int y, int w, int h, int blur) {
    if (!fb_back) return;
    /* Draw progressively fainter rectangles around the card */
    for (int b = 0; b < blur; b++) {
        int alpha = 40 - (b * 40 / blur);
        if (alpha <= 0) break;
        uint32_t shadow_color = ((uint32_t)alpha << 24) | 0x00000000;
        /* Top shadow */
        for (int i = x - b; i < x + w + b; i++) {
            int py = y - b;
            if (i >= 0 && i < (int)fb_w && py >= 0 && py < (int)fb_h) {
                fb_set_pixel(i, py, fb_blend(fb_get_pixel(i, py), shadow_color));
            }
        }
        /* Bottom shadow */
        for (int i = x - b; i < x + w + b; i++) {
            int py = y + h + b;
            if (i >= 0 && i < (int)fb_w && py >= 0 && py < (int)fb_h) {
                fb_set_pixel(i, py, fb_blend(fb_get_pixel(i, py), shadow_color));
            }
        }
        /* Left shadow */
        for (int i = y - b; i < y + h + b; i++) {
            int px = x - b;
            if (px >= 0 && px < (int)fb_w && i >= 0 && i < (int)fb_h) {
                fb_set_pixel(px, i, fb_blend(fb_get_pixel(px, i), shadow_color));
            }
        }
        /* Right shadow */
        for (int i = y - b; i < y + h + b; i++) {
            int px = x + w + b;
            if (px >= 0 && px < (int)fb_w && i >= 0 && i < (int)fb_h) {
                fb_set_pixel(px, i, fb_blend(fb_get_pixel(px, i), shadow_color));
            }
        }
    }
}

/* ===== ENHANCED CARD RENDERING ===== */
/* Draw a professional card with shadow, glassmorphism body, focus glow,
 * title bar, and close button. */
void ui_draw_card(int x, int y, int w, int h, int focused) {
    /* Drop shadow */
    draw_shadow(x, y, w, h, 8);

    /* Card body with glassmorphism */
    fb_draw_rounded(x, y, w, h, 16,
                    UI_GLASS_BG,
                    focused ? UI_ACTIVE_GLOW : UI_GLASS_BORDER);

    /* If focused, add extra glow */
    if (focused) {
        for (int b = 0; b < 4; b++) {
            int alpha = 30 - b * 7;
            uint32_t glow = ((uint32_t)alpha << 24) | 0x0022D3EE;
            fb_draw_rounded(x - b, y - b, w + 2*b, h + 2*b, 16 + b,
                            0x00000000, glow);
        }
    }

    /* Title bar background (top 36px, slightly darker) */
    fb_fill_rect(x + 1, y + 1, w - 2, 35, 0xE00E1422);

    /* Close button (top-right): red circle with X */
    int btn_x = x + w - 22;
    int btn_y = y + 10;
    fb_draw_circle(btn_x + 6, btn_y + 6, 7, 0x80F87171);
    fb_draw_string(btn_x + 2, btn_y, "x", 0xFFFFFFFF);

    /* Minimize button (left of close): yellow circle */
    btn_x -= 20;
    fb_draw_circle(btn_x + 6, btn_y + 6, 7, 0x80FBBF24);

    /* Focus indicator dot (left of minimize): green circle */
    btn_x -= 20;
    fb_draw_circle(btn_x + 6, btn_y + 6, 7, focused ? 0x804ADE80 : 0x40475569);
}

/* ===== FLOATING TOP BAR ===== */
/* Centered floating bar (like the dock), not full-width */
void ui_render_status_bar(void) {
    char left_buf[32];
    char right_buf[256];
    int rlen = 0;

    /* Left: "lestraOS" logo */
    strcpy(left_buf, "lestraOS");
    int left_w = fb_text_width(left_buf);

    /* Right: status info */
    uint8_t hour, min, sec;
    uint16_t year;
    uint8_t month, day;
    rtc_get_time(&hour, &min, &sec);
    rtc_get_date(&year, &month, &day);
    rlen += ksnprintf(right_buf, sizeof(right_buf),
                     "%02u:%02u", (unsigned)hour, (unsigned)min);

    int bat = battery_get_percent();
    if (bat >= 0) {
        rlen += ksnprintf(right_buf + rlen, sizeof(right_buf) - rlen,
                         "  BAT %u%%", (unsigned)bat);
        if (battery_is_charging()) { right_buf[rlen++] = '+'; right_buf[rlen] = '\0'; }
    }

    int temp = temp_get_cpu();
    if (temp >= 0) {
        rlen += ksnprintf(right_buf + rlen, sizeof(right_buf) - rlen,
                         "  %uC", (unsigned)temp);
    }

    rlen += ksnprintf(right_buf + rlen, sizeof(right_buf) - rlen,
                     "  %uMB",
                     (unsigned)(pmm_get_free() / (1024 * 1024)));

    if (net_is_up()) {
        if (wifi_is_connected()) {
            const char* ssid = wifi_get_connected_ssid();
            rlen += ksnprintf(right_buf + rlen, sizeof(right_buf) - rlen,
                             "  %s", ssid ? ssid : "WiFi");
        } else {
            rlen += ksnprintf(right_buf + rlen, sizeof(right_buf) - rlen, "  ETH");
        }
    } else {
        rlen += ksnprintf(right_buf + rlen, sizeof(right_buf) - rlen, "  OFF");
    }

    int right_w = fb_text_width(right_buf);

    /* Floating bar: centered, width = left + gap + right + padding */
    int bar_w = left_w + 24 + right_w + 32;
    int bar_x = (int)fb_w / 2 - bar_w / 2;
    int bar_y = 12;
    int bar_h = 28;

    /* Shadow */
    draw_shadow(bar_x, bar_y, bar_w, bar_h, 4);

    /* Floating pill background */
    fb_draw_rounded(bar_x, bar_y, bar_w, bar_h, 14,
                    UI_DOCK_BG, UI_DOCK_BORDER);

    /* Left: logo */
    fb_draw_string(bar_x + 16, bar_y + 6, left_buf, UI_ACCENT);

    /* Separator dot */
    fb_draw_circle(bar_x + 16 + left_w + 8, bar_y + 14, 2, UI_TEXT_FAINT);

    /* Right: status */
    fb_draw_string(bar_x + 16 + left_w + 20, bar_y + 6, right_buf, UI_TEXT_PRIMARY);
}

/* ===== ENHANCED DOCK ===== */
#define DOCK_H 60
#define DOCK_ICON 44
#define DOCK_GAP 10
#define DOCK_PAD 14
#define DOCK_BOTTOM 12

struct dock_app_new {
    const char* glyph;
    const char* label;
    uint32_t tint;
};

static struct dock_app_new dock_apps_new[] = {
    { ">_", "Term",   0xCC22D3EE },
    { "AI", "AI",     0xCC67E8F9 },
    { "Ed", "Edit",   0xCC4ADE80 },
    { "M",  "Media",  0xCCF87171 },
    { "F",  "Files",  0xCCFBBF24 },
    { "i",  "About",  0xCC94A3B8 },
    { "?",  "Help",   0xCC67E8F9 },
    { "X",  "Exit",   0xCCF87171 },
};
#define NUM_DOCK_NEW (sizeof(dock_apps_new)/sizeof(dock_apps_new[0]))

static int dock_hover_idx = -1;
static struct widget* dock_widgets[8] = {0};

extern struct widget* terminal_create(int x, int y);
extern struct widget* ailab_create(int x, int y);
extern struct widget* about_create(int x, int y);
extern struct widget* help_create(int x, int y);
extern struct widget* editor_create(int x, int y);
extern struct widget* media_create(int x, int y);
extern void compositor_add(struct widget* w);
extern void compositor_bring_to_front(struct widget* w);
extern void compositor_quit(void);

static void dock_launch_new(int idx) {
    struct widget* w = NULL;
    switch (idx) {
        case 0:
            if (!dock_widgets[0]) { dock_widgets[0] = terminal_create(180, 40); compositor_add(dock_widgets[0]); }
            w = dock_widgets[0]; break;
        case 1:
            if (!dock_widgets[1]) { dock_widgets[1] = ailab_create(200, 60); compositor_add(dock_widgets[1]); }
            w = dock_widgets[1]; break;
        case 2:
            if (!dock_widgets[2]) { dock_widgets[2] = editor_create(200, 60); compositor_add(dock_widgets[2]); }
            w = dock_widgets[2]; break;
        case 3:
            if (!dock_widgets[3]) { dock_widgets[3] = media_create(250, 100); compositor_add(dock_widgets[3]); }
            w = dock_widgets[3]; break;
        case 5:
            if (!dock_widgets[5]) { dock_widgets[5] = about_create((int)fb_w/2-190, 200); compositor_add(dock_widgets[5]); }
            w = dock_widgets[5]; break;
        case 6:
            if (!dock_widgets[6]) { dock_widgets[6] = help_create((int)fb_w/2-200, 180); compositor_add(dock_widgets[6]); }
            w = dock_widgets[6]; break;
        case 7:
            compositor_quit(); return;
    }
    if (w) { w->visible = 1; compositor_bring_to_front(w); }
}

static void dock_get_geom(int* dx, int* dw) {
    int icons_w = NUM_DOCK_NEW * (DOCK_ICON + DOCK_GAP) - DOCK_GAP;
    *dw = icons_w + 2 * DOCK_PAD + 70;  /* +70 for clock */
    *dx = ((int)fb_w - *dw) / 2;
}

void ui_render_dock(void) {
    int dx, dw;
    dock_get_geom(&dx, &dw);
    int dy = (int)fb_h - DOCK_H - DOCK_BOTTOM;

    /* Dock shadow */
    draw_shadow(dx, dy, dw, DOCK_H, 6);

    /* Dock body */
    fb_draw_rounded(dx, dy, dw, DOCK_H, 16,
                    UI_DOCK_BG, UI_DOCK_BORDER);

    /* Icons */
    int ix = dx + DOCK_PAD;
    int iy = dy + (DOCK_H - DOCK_ICON) / 2;

    for (int i = 0; i < (int)NUM_DOCK_NEW; i++) {
        int size = DOCK_ICON;
        int offset = 0;

        /* Magnification on hover */
        if (dock_hover_idx == i) {
            size = DOCK_ICON + 6;
            offset = -3;
        }

        /* Icon background */
        uint32_t tint = dock_apps_new[i].tint;
        if (dock_hover_idx == i) {
            tint = ((tint & 0xFF000000) ? tint : 0xCC22D3EE) | 0x20000000;
        }

        fb_draw_rounded(ix, iy + offset, size, size, 10,
                        tint, 0x3022D3EE);

        /* Real icon image - blit raw RGB (48x48 scaled down to fit 44px slot) */
        const uint8_t* icon_data = get_icon_data(i);
        if (icon_data) {
            int icon_off = (size - 32) / 2;
            /* Render at 32x32 (subsample from 48x48) */
            for (int row = 0; row < 32; row++) {
                for (int col = 0; col < 32; col++) {
                    int sx = (col * 48) / 32;
                    int sy = (row * 48) / 32;
                    const uint8_t* src = &icon_data[(sy * 48 + sx) * 3];
                    uint8_t r = src[0], g = src[1], b = src[2];
                    if (r == 0x01 && g == 0x01 && b == 0x01) continue;
                    uint32_t color = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                    fb_set_pixel(ix + icon_off + col, iy + offset + icon_off + row, color);
                }
            }
        }

        ix += DOCK_ICON + DOCK_GAP;
    }

    /* Clock */
    uint8_t h, m, s;
    rtc_get_time(&h, &m, &s);
    char clock[12];
    ksnprintf(clock, sizeof(clock), "%02u:%02u", (unsigned)h, (unsigned)m);
    int cw = fb_text_width(clock);
    fb_draw_string(dx + dw - DOCK_PAD - cw,
                   dy + (DOCK_H - 16) / 2, clock, UI_TEXT_PRIMARY);
}

int ui_dock_handle_event(struct event* e) {
    if (e->type == EV_MOUSE_MOVE) {
        int dx, dw;
        dock_get_geom(&dx, &dw);
        int dy = (int)fb_h - DOCK_H - DOCK_BOTTOM;
        if (e->mouse.y >= dy && e->mouse.y < dy + DOCK_H &&
            e->mouse.x >= dx && e->mouse.x < dx + dw) {
            int ix = dx + DOCK_PAD;
            dock_hover_idx = -1;
            for (int i = 0; i < (int)NUM_DOCK_NEW; i++) {
                if (e->mouse.x >= ix && e->mouse.x < ix + DOCK_ICON) {
                    dock_hover_idx = i;
                    break;
                }
                ix += DOCK_ICON + DOCK_GAP;
            }
        } else {
            dock_hover_idx = -1;
        }
        return 0;
    }
    if (e->type == EV_MOUSE_DOWN) {
        int dx, dw;
        dock_get_geom(&dx, &dw);
        int dy = (int)fb_h - DOCK_H - DOCK_BOTTOM;
        if (e->mouse.y >= dy && e->mouse.y < dy + DOCK_H &&
            e->mouse.x >= dx && e->mouse.x < dx + dw) {
            int ix = dx + DOCK_PAD;
            for (int i = 0; i < (int)NUM_DOCK_NEW; i++) {
                if (e->mouse.x >= ix && e->mouse.x < ix + DOCK_ICON) {
                    dock_launch_new(i);
                    return 1;
                }
                ix += DOCK_ICON + DOCK_GAP;
            }
        }
    }
    return 0;
}

/* ===== ENHANCED DESKTOP ICONS ===== */
struct desktop_icon_new {
    const char* glyph;
    const char* label;
    uint32_t tint;
    int x, y;
    int hovered;
};

static struct desktop_icon_new desk_icons[] = {
    { ">_", "Terminal",   0xCC22D3EE, 0,0, 0 },
    { "AI", "AI Lab",     0xCC67E8F9, 0,0, 0 },
    { "Ed", "Editor",     0xCC4ADE80, 0,0, 0 },
    { "M",  "Media",      0xCCF87171, 0,0, 0 },
    { "F",  "Files",      0xCCFBBF24, 0,0, 0 },
    { "#",  "Settings",   0xCC94A3B8, 0,0, 0 },
};
#define NUM_DESK_ICONS (sizeof(desk_icons)/sizeof(desk_icons[0]))

static int desk_icons_init = 0;

static void desk_layout(void) {
    for (int i = 0; i < (int)NUM_DESK_ICONS; i++) {
        desk_icons[i].x = 32;
        desk_icons[i].y = 60 + i * (64 + 16 + 24);
    }
    desk_icons_init = 1;
}

void ui_render_desktop_icons(void) {
    if (!desk_icons_init) desk_layout();

    for (int i = 0; i < (int)NUM_DESK_ICONS; i++) {
        struct desktop_icon_new* ic = &desk_icons[i];
        int size = 64;
        int offset = 0;

        if (ic->hovered) {
            size = 68;
            offset = -2;
        }

        /* Shadow */
        draw_shadow(ic->x, ic->y, size, size, 4);

        /* Icon body */
        fb_draw_rounded(ic->x + offset, ic->y + offset, size, size, 14,
                        ic->hovered ? (ic->tint | 0x30000000) : ic->tint,
                        ic->hovered ? UI_HOVER_GLOW : 0x3022D3EE);

        /* Real bitmap icon - blit raw RGB image at 2x scale (48->96px, scaled to fit 64px) */
        const uint8_t* icon_data = get_icon_data(i);
        if (icon_data) {
            /* Scale 48x48 to ~64x64 (1.33x, use fill_rect for 1.33x approximation = 1x + pad) */
            /* For clean rendering, just blit at 1x (48px) centered in the 64px icon area */
            int icon_off = (size - 48) / 2;
            for (int row = 0; row < 48; row++) {
                for (int col = 0; col < 48; col++) {
                    const uint8_t* src = &icon_data[(row * 48 + col) * 3];
                    uint8_t r = src[0], g = src[1], b = src[2];
                    /* Skip transparent pixels (key = 0x01,0x01,0x01) */
                    if (r == 0x01 && g == 0x01 && b == 0x01) continue;
                    uint32_t color = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                    fb_set_pixel(ic->x + offset + icon_off + col, ic->y + offset + icon_off + row, color);
                }
            }
        }

        /* Label */
        int lw = fb_text_width(ic->label);
        int lx = ic->x + (64 - lw) / 2;
        int ly = ic->y + 68;
        /* Label background */
        fb_fill_rect(lx - 4, ly, lw + 8, 16, 0x80050608);
        fb_draw_string(lx, ly, ic->label, UI_TEXT_PRIMARY);
    }
}

int ui_desktop_icons_handle_click(int x, int y) {
    if (!desk_icons_init) desk_layout();
    for (int i = 0; i < (int)NUM_DESK_ICONS; i++) {
        struct desktop_icon_new* ic = &desk_icons[i];
        if (x >= ic->x && x < ic->x + 64 &&
            y >= ic->y && y < ic->y + 84) {
            /* Launch the same app as dock index i */
            dock_launch_new(i);
            return 1;
        }
    }
    return 0;
}

void ui_desktop_icons_handle_move(int x, int y) {
    if (!desk_icons_init) desk_layout();
    for (int i = 0; i < (int)NUM_DESK_ICONS; i++) {
        desk_icons[i].hovered = (x >= desk_icons[i].x &&
                                 x < desk_icons[i].x + 64 &&
                                 y >= desk_icons[i].y &&
                                 y < desk_icons[i].y + 84);
    }
}

/* ===== WALLPAPER BRANDING ===== */
/* Draw the real wallpaper image + LestraOS branding text */
void ui_render_wallpaper(void) {
    /* Use the Meet Yugi wallpaper from wallpaper.c */
    extern void wallpaper_render(void);
    wallpaper_render();

    /* Draw "lestraOS" branding text on top of wallpaper, faint */
    const char* brand = "lestraOS";
    int char_w = 8 * 6;  /* 6x scale = 48px per char */
    int total_w = strlen(brand) * char_w;
    int bx = (int)fb_w / 2 - total_w / 2;
    int by = (int)fb_h / 2 - 48;

    uint32_t faint = 0x2022D3EE;  /* low alpha cyan */
    int x = bx;
    for (const char* p = brand; *p; p++) {
        fb_draw_char_scale(x, by, *p, faint, 6);
        x += char_w;
    }

    /* Subtitle */
    const char* sub = "by Lee Muriithi Kingori";
    int sub_w = fb_text_width(sub) * 2;
    int sx = (int)fb_w / 2 - sub_w / 2;
    int sy = by + 100;
    faint = 0x1894A3B8;
    x = sx;
    for (const char* p = sub; *p; p++) {
        fb_draw_char_scale(x, sy, *p, faint, 2);
        x += 16;
    }
}

/* Get icon data by index (0=terminal, 1=ai, 2=editor, 3=media, 4=files,
 * 5=settings, 6=help, 7=about). Returns 48x48 RGB888 with transparent
 * key 0x010101. */
const uint8_t* get_icon_data(int idx) {
    switch (idx) {
        case 0: return icon_terminal_data;
        case 1: return icon_ailab_data;
        case 2: return icon_editor_data;
        case 3: return icon_media_data;
        case 4: return icon_files_data;
        case 5: return icon_settings_data;
        case 6: return icon_help_data;
        case 7: return icon_about_data;
        default: return icon_terminal_data;
    }
}

/* ===== MINI MUSIC PLAYER ===== */
/* A small popup that appears above the dock when audio is playing. */
static int music_playing = 0;
static char music_track[64] = "";
static uint64_t music_start_time = 0;

void music_set_playing(const char* track_name) {
    music_playing = 1;
    if (track_name) {
        strncpy(music_track, track_name, sizeof(music_track) - 1);
        music_track[sizeof(music_track) - 1] = '\0';
    } else {
        music_track[0] = '\0';
    }
    music_start_time = timer_get_ms();
}

void music_set_stopped(void) {
    music_playing = 0;
    music_track[0] = '\0';
}

int music_is_playing(void) {
    return music_playing;
}

void ui_render_mini_player(void) {
    if (!music_playing) return;

    int player_w = 320;
    int player_h = 72;
    int px = (int)fb_w / 2 - player_w / 2;
    int py = (int)fb_h - 140;  /* above the dock */

    /* Shadow */
    draw_shadow(px, py, player_w, player_h, 6);

    /* Body */
    fb_draw_rounded(px, py, player_w, player_h, 14,
                    0xCC0E1422, 0x4022D3EE);

    /* Play icon (triangle) */
    int icon_x = px + 16;
    int icon_y = py + (player_h - 24) / 2;
    for (int dy = 0; dy < 24; dy++) {
        int width = dy + 1;
        for (int dx = 0; dx < width && dx < 20; dx++) {
            fb_set_pixel(icon_x + dx, icon_y + dy, UI_ACCENT);
        }
    }

    /* Track name */
    fb_draw_string(px + 56, py + 12, music_track, UI_TEXT_PRIMARY);

    /* Progress bar */
    uint64_t elapsed = timer_get_ms() - music_start_time;
    int progress = (int)((elapsed / 1000) % 100);
    int bar_x = px + 56;
    int bar_y = py + 40;
    int bar_w = player_w - 72;
    fb_fill_rect(bar_x, bar_y, bar_w, 3, UI_TEXT_FAINT);
    fb_fill_rect(bar_x, bar_y, (bar_w * progress) / 100, 3, UI_ACCENT);

    /* Time labels */
    char time_buf[16];
    uint64_t secs = elapsed / 1000;
    ksnprintf(time_buf, sizeof(time_buf), "0:%02u", (unsigned)secs);
    fb_draw_string(bar_x, py + 48, time_buf, UI_TEXT_MUTED);
    ksnprintf(time_buf, sizeof(time_buf), "0:%02u", (unsigned)(secs + 1));
    fb_draw_string(bar_x + bar_w - 32, py + 48, time_buf, UI_TEXT_MUTED);
}

/* ===== NOTIFICATION SYSTEM ===== */
#define MAX_NOTIFICATIONS 4
struct notification {
    char text[128];
    uint64_t timestamp;
    int active;
};
static struct notification notifications[MAX_NOTIFICATIONS];

void ui_notify(const char* text) {
    /* Shift all notifications up */
    for (int i = MAX_NOTIFICATIONS - 1; i > 0; i--) {
        notifications[i] = notifications[i - 1];
    }
    strncpy(notifications[0].text, text, sizeof(notifications[0].text) - 1);
    notifications[0].text[sizeof(notifications[0].text) - 1] = '\0';
    notifications[0].timestamp = timer_get_ms();
    notifications[0].active = 1;
}

void ui_render_notifications(void) {
    uint64_t now = timer_get_ms();
    int notif_x = (int)fb_w - 280;
    int notif_base_y = 56;
    int notif_w = 260;
    int notif_h = 40;
    uint64_t POP_IN_MS = 250;
    uint64_t FADE_OUT_MS = 400;
    uint64_t LIFETIME_MS = 4000;

    for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
        if (!notifications[i].active) continue;

        uint64_t age = now - notifications[i].timestamp;

        /* Auto-dismiss */
        if (age > LIFETIME_MS) {
            notifications[i].active = 0;
            continue;
        }

        /* Pop-in animation: slide down from above */
        int y_offset = 0;
        int alpha = 255;
        if (age < POP_IN_MS) {
            /* Sliding in from top */
            int progress = (int)(age * 1000 / POP_IN_MS);
            y_offset = -40 + (40 * progress) / 1000;
            alpha = (255 * progress) / 1000;
        } else if (age > LIFETIME_MS - FADE_OUT_MS) {
            /* Fading out */
            uint64_t fade_age = age - (LIFETIME_MS - FADE_OUT_MS);
            int progress = (int)(fade_age * 1000 / FADE_OUT_MS);
            alpha = 255 - (255 * progress) / 1000;
            y_offset = -(8 * progress) / 1000;  /* slide up slightly */
        }

        int y = notif_base_y + i * (notif_h + 8) + y_offset;

        /* Shadow */
        draw_shadow(notif_x, y, notif_w, notif_h, 4);

        /* Body with alpha */
        uint32_t bg_alpha = (0xCC * alpha) / 255;
        uint32_t border_alpha = (0x30 * alpha) / 255;
        fb_draw_rounded(notif_x, y, notif_w, notif_h, 10,
                        (bg_alpha << 24) | 0x000E1422,
                        (border_alpha << 24) | 0x0022D3EE);

        /* Accent dot */
        uint32_t dot_color = ((alpha / 2) << 24) | (UI_ACCENT & 0x00FFFFFF);
        fb_draw_circle(notif_x + 12, y + notif_h / 2, 4, dot_color);

        /* Text */
        uint32_t text_color = (alpha << 24) | (UI_TEXT_PRIMARY & 0x00FFFFFF);
        fb_draw_string(notif_x + 24, y + 12, notifications[i].text, text_color);
    }
}

/* ===== SETTINGS STATE (with on-disk persistence) =====
 *
 * The four settings (brightness, volume, adblock, dark_mode) used to
 * be in-memory only — every reboot reset them to the defaults below.
 * They now persist to a flat key=value file at /etc/settings.conf on
 * the VFS. On any settings_set_*() call we rewrite the whole file
 * (it's tiny — 4 lines). On first access (lazy via settings_load()),
 * we read the file back and call the setters to restore state.
 *
 * NOTE: /etc is memfs (volatile) in the default boot, so this does
 * NOT survive a reboot YET — but the mechanism is in place so when
 * an ext2/disk fs is mounted at / (or /etc is bind-mounted onto a
 * disk-backed fs), persistence Just Works. The file format is plain
 * text on purpose so it can be inspected / edited by hand. */
static int settings_brightness = 80;  /* 0-100 */
static int settings_volume = 75;      /* 0-100 */
static int settings_adblock = 1;
static int settings_dark_mode = 1;

#define SETTINGS_PATH  "/etc/settings.conf"
static int settings_loaded = 0;

/* Parse one "key=value\n" line and apply it via the appropriate
 * setter. Tolerates trailing whitespace / missing newline. */
static void settings_apply_line(char* line) {
    /* Strip trailing newline / CR. */
    int len = (int)strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                       line[len-1] == ' '  || line[len-1] == '\t')) {
        line[--len] = '\0';
    }
    if (len == 0) return;

    /* Split on '='. */
    char* eq = strchr(line, '=');
    if (!eq) return;
    *eq = '\0';
    const char* key = line;
    const char* val = eq + 1;
    if (!*key || !*val) return;

    /* atoi-equivalent (libc isn't linked into the kernel). */
    int v = 0;
    int sign = 1;
    const char* p = val;
    if (*p == '-') { sign = -1; p++; }
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    v *= sign;

    if (strcmp(key, "brightness") == 0) {
        settings_brightness = v;
        if (settings_brightness < 10) settings_brightness = 10;
        if (settings_brightness > 100) settings_brightness = 100;
    } else if (strcmp(key, "volume") == 0) {
        settings_volume = v;
        if (settings_volume < 0) settings_volume = 0;
        if (settings_volume > 100) settings_volume = 100;
    } else if (strcmp(key, "adblock") == 0) {
        settings_adblock = v ? 1 : 0;
    } else if (strcmp(key, "dark_mode") == 0) {
        settings_dark_mode = v ? 1 : 0;
    }
    /* Unknown keys are silently ignored — forward-compat. */
}

/* Read /etc/settings.conf (if it exists) and apply each line. Called
 * lazily on the first settings_get_*() call so we don't pay the VFS
 * cost at boot until the GUI actually needs a setting. */
static void settings_load(void) {
    if (settings_loaded) return;
    settings_loaded = 1;

    int fd = vfs_open(SETTINGS_PATH, O_RDONLY);
    if (fd < 0) {
        /* File doesn't exist yet — defaults stay. This is the normal
         * first-boot path; the first settings_set_*() will create it. */
        return;
    }
    static char buf[512];
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(buf) - 1) {
        ssize_t n = vfs_read(fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    vfs_close(fd);
    buf[total] = '\0';

    /* Walk line-by-line. */
    char* p = buf;
    while (p && *p) {
        char* nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        settings_apply_line(p);
        if (!nl) break;
        p = nl + 1;
    }
    pr_info("settings: loaded %s (brightness=%d volume=%d adblock=%d dark_mode=%d)\n",
            SETTINGS_PATH, settings_brightness, settings_volume,
            settings_adblock, settings_dark_mode);
}

/* Write the current settings to /etc/settings.conf as flat key=value
 * lines. Creates /etc if missing (vfs_open with O_CREAT will fail if
 * the parent dir doesn't exist, so we vfs_mkdir it first as a best
 * effort — memfs allows mkdir of an existing dir to fail silently). */
static void settings_persist(void) {
    /* Build the file contents in a stack buffer (always small). */
    char buf[256];
    int len = 0;
    len += ksnprintf(buf + len, sizeof(buf) - len,
                     "brightness=%d\n", settings_brightness);
    len += ksnprintf(buf + len, sizeof(buf) - len,
                     "volume=%d\n", settings_volume);
    len += ksnprintf(buf + len, sizeof(buf) - len,
                     "adblock=%d\n", settings_adblock);
    len += ksnprintf(buf + len, sizeof(buf) - len,
                     "dark_mode=%d\n", settings_dark_mode);

    /* Make sure /etc exists (best effort — ignore EEXIST). */
    vfs_mkdir("/etc", 0755);

    int fd = vfs_open(SETTINGS_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        pr_info("settings: cannot open %s for write (rc=%d)\n",
                SETTINGS_PATH, fd);
        return;
    }
    ssize_t wrote = vfs_write(fd, buf, len);
    vfs_close(fd);
    if (wrote < 0) {
        pr_info("settings: write to %s failed (rc=%d)\n",
                SETTINGS_PATH, (int)wrote);
    }
}

int settings_get_brightness(void) { settings_load(); return settings_brightness; }
int settings_get_volume(void)     { settings_load(); return settings_volume; }
int settings_get_adblock(void)    { settings_load(); return settings_adblock; }
int settings_get_dark_mode(void)  { settings_load(); return settings_dark_mode; }

void settings_set_brightness(int v) {
    settings_load();
    settings_brightness = v;
    if (settings_brightness < 10) settings_brightness = 10;
    if (settings_brightness > 100) settings_brightness = 100;
    settings_persist();
}
void settings_set_volume(int v) {
    settings_load();
    settings_volume = v;
    if (settings_volume < 0) settings_volume = 0;
    if (settings_volume > 100) settings_volume = 100;
    settings_persist();
}
void settings_set_adblock(int v) {
    settings_load();
    settings_adblock = v ? 1 : 0;
    settings_persist();
}
void settings_set_dark_mode(int v) {
    settings_load();
    settings_dark_mode = v ? 1 : 0;
    settings_persist();
}
