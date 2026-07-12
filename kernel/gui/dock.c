/*
 * Lestra OS - Professional Desktop Dock
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * A floating bar centered at the bottom of the screen with app icons.
 * This is the "Windows/Linux-like" dock the user requested — a
 * professional-quality UI element with:
 *   - Centered floating bar (not docked to edge)
 *   - App icons that launch programs on click
 *   - Hover highlight effect
 *   - Active app indicator (dot below running apps)
 *   - Clock display on the right
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/timer.h>
#include <lestra/printk.h>
#include <string.h>

/* Dock constants */
#define DOCK_HEIGHT 56
#define DOCK_ICON_SIZE 40
#define DOCK_ICON_GAP 12
#define DOCK_BOTTOM_MARGIN 16
#define DOCK_PADDING 12

/* Dock app definitions */
#define MAX_DOCK_APPS 8
struct dock_app {
    const char* glyph;
    const char* label;
    int x;  /* computed position */
};

static struct dock_app dock_apps[MAX_DOCK_APPS] = {
    { ">_", "Terminal" },
    { "AI", "AI Lab" },
    { "Ed", "Editor" },
    { "M",  "Media" },
    { "F",  "Files" },
    { "i",  "About" },
    { "?",  "Help" },
    { "X",  "Exit" },
};
#define NUM_DOCK_APPS 8

/* Track which apps are running */
static int app_running[MAX_DOCK_APPS] = {0};

/* External: app launchers */
extern struct widget* terminal_create(int x, int y);
extern struct widget* ailab_create(int x, int y);
extern struct widget* about_create(int x, int y);
extern struct widget* help_create(int x, int y);
extern struct widget* editor_create(int x, int y);
extern struct widget* media_create(int x, int y);
extern void compositor_add(struct widget* w);
extern void compositor_bring_to_front(struct widget* w);
extern void compositor_quit(void);

static struct widget* w_term = NULL;
static struct widget* w_ailab = NULL;
static struct widget* w_about = NULL;
static struct widget* w_help = NULL;
static struct widget* w_editor = NULL;
static struct widget* w_media = NULL;

static int dock_hovered = -1;

/* Compute dock geometry */
static void dock_get_geometry(int* dock_x, int* dock_w) {
    int total_icons = NUM_DOCK_APPS * (DOCK_ICON_SIZE + DOCK_ICON_GAP) - DOCK_ICON_GAP;
    *dock_w = total_icons + 2 * DOCK_PADDING + 80;  /* +80 for clock area */
    *dock_x = ((int)fb_w - *dock_w) / 2;
}

/* Launch or focus an app */
static void dock_launch(int idx) {
    struct widget* w = NULL;
    switch (idx) {
        case 0:
            if (!w_term) { w_term = terminal_create(180, 40); compositor_add(w_term); }
            w = w_term; break;
        case 1:
            if (!w_ailab) { w_ailab = ailab_create(200, 60); compositor_add(w_ailab); }
            w = w_ailab; break;
        case 2:
            if (!w_editor) { w_editor = editor_create(200, 60); compositor_add(w_editor); }
            w = w_editor; break;
        case 3:
            if (!w_media) { w_media = media_create(250, 100); compositor_add(w_media); }
            w = w_media; break;
        case 4:
            /* Files — not yet implemented as a card */
            return;
        case 5:
            if (!w_about) { w_about = about_create((int)fb_w/2 - 190, 200); compositor_add(w_about); }
            w = w_about; break;
        case 6:
            if (!w_help) { w_help = help_create((int)fb_w/2 - 200, 180); compositor_add(w_help); }
            w = w_help; break;
        case 7:
            /* Exit — quit compositor */
            compositor_quit();
            return;
    }
    if (w) {
        w->visible = 1;
        app_running[idx] = 1;
        compositor_bring_to_front(w);
    }
}

/* Render the dock. Called from compositor after widgets. */
void dock_render(void) {
    int dock_x, dock_w;
    dock_get_geometry(&dock_x, &dock_w);
    int dock_y = (int)fb_h - DOCK_HEIGHT - DOCK_BOTTOM_MARGIN;

    /* Dock background: translucent rounded bar */
    fb_draw_rounded(dock_x, dock_y, dock_w, DOCK_HEIGHT, 14,
                    0xD90E1422, 0x3022D3EE);

    /* Draw app icons */
    int icon_x = dock_x + DOCK_PADDING;
    int icon_y = dock_y + (DOCK_HEIGHT - DOCK_ICON_SIZE) / 2;

    for (int i = 0; i < NUM_DOCK_APPS; i++) {
        dock_apps[i].x = icon_x;

        /* Hover highlight */
        if (dock_hovered == i) {
            fb_draw_rounded(icon_x - 2, icon_y - 2,
                            DOCK_ICON_SIZE + 4, DOCK_ICON_SIZE + 4, 10,
                            0x4022D3EE, 0x4022D3EE);
        }

        /* Icon background */
        uint32_t icon_bg = (dock_hovered == i) ? 0x8022D3EE : 0x60121828;
        fb_draw_rounded(icon_x, icon_y, DOCK_ICON_SIZE, DOCK_ICON_SIZE, 8,
                        icon_bg, 0x3022D3EE);

        /* Glyph (centered, 2x scale) */
        int gw = fb_text_width(dock_apps[i].glyph);
        int gx = icon_x + (DOCK_ICON_SIZE - gw * 2) / 2;
        int gy = icon_y + (DOCK_ICON_SIZE - 32) / 2;
        for (const char* p = dock_apps[i].glyph; *p; p++) {
            fb_draw_char_scale(gx, gy, *p, UI_TEXT_PRIMARY, 2);
            gx += 8 * 2;
        }

        /* Running indicator (dot below icon) */
        if (app_running[i]) {
            int dot_x = icon_x + DOCK_ICON_SIZE / 2;
            int dot_y = dock_y + DOCK_HEIGHT - 6;
            fb_draw_circle(dot_x, dot_y, 3, UI_ACCENT);
        }

        icon_x += DOCK_ICON_SIZE + DOCK_ICON_GAP;
    }

    /* Clock on the right side of dock — use RTC for real time */
    extern void rtc_get_time(uint8_t*, uint8_t*, uint8_t*);
    uint8_t r_hour, r_min, r_sec;
    rtc_get_time(&r_hour, &r_min, &r_sec);
    char clock[16];
    ksnprintf(clock, sizeof(clock), "%u:%02u:%02u",
              (unsigned)r_hour, (unsigned)r_min, (unsigned)r_sec);
    int cw = fb_text_width(clock);
    fb_draw_string(dock_x + dock_w - DOCK_PADDING - cw,
                   dock_y + (DOCK_HEIGHT - 16) / 2,
                   clock, UI_TEXT_PRIMARY);
}

/* Check if a point is inside the dock. Returns the app index or -1. */
static int dock_hit(int mx, int my) {
    int dock_x, dock_w;
    dock_get_geometry(&dock_x, &dock_w);
    int dock_y = (int)fb_h - DOCK_HEIGHT - DOCK_BOTTOM_MARGIN;

    if (my < dock_y || my >= dock_y + DOCK_HEIGHT) return -1;
    if (mx < dock_x || mx >= dock_x + dock_w) return -1;

    /* Check which icon */
    int icon_x = dock_x + DOCK_PADDING;
    for (int i = 0; i < NUM_DOCK_APPS; i++) {
        if (mx >= icon_x && mx < icon_x + DOCK_ICON_SIZE) {
            return i;
        }
        icon_x += DOCK_ICON_SIZE + DOCK_ICON_GAP;
    }
    return -1;
}

/* Handle a mouse event. Returns 1 if consumed. */
int dock_handle_event(struct event* e) {
    if (e->type == EV_MOUSE_MOVE) {
        dock_hovered = dock_hit(e->mouse.x, e->mouse.y);
        return 0;  /* don't consume — other widgets need move events */
    }
    if (e->type == EV_MOUSE_DOWN) {
        int idx = dock_hit(e->mouse.x, e->mouse.y);
        if (idx >= 0) {
            dock_launch(idx);
            return 1;
        }
    }
    return 0;
}
