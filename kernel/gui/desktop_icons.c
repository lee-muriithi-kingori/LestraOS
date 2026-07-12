/*
 * Lestra OS - Desktop Icons
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Clickable app icons rendered on the desktop background. Each icon is
 * a 64×64 glyph + label. Clicking launches the corresponding app card.
 *
 * Icons are arranged in a vertical column on the left side of the screen,
 * starting 60px from the top with 24px spacing.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/printk.h>
#include <string.h>

#define ICON_SIZE 64
#define ICON_GAP 24
#define ICON_AREA_X 32
#define ICON_AREA_Y 60
#define ICON_LABEL_H 16

/* Forward declarations for app creators */
extern struct widget* terminal_create(int x, int y);
extern struct widget* ailab_create(int x, int y);
extern struct widget* about_create(int x, int y);
extern struct widget* help_create(int x, int y);
extern struct widget* editor_create(int x, int y);
extern struct widget* media_create(int x, int y);
extern void compositor_add(struct widget* w);
extern void compositor_bring_to_front(struct widget* w);

/* Track created widgets to avoid duplicates */
static struct widget* w_terminal = NULL;
static struct widget* w_ailab = NULL;
static struct widget* w_about = NULL;
static struct widget* w_help = NULL;
static struct widget* w_editor = NULL;
static struct widget* w_media = NULL;

/* Desktop icon definition */
struct desktop_icon {
    const char* glyph;    /* 1-3 char glyph drawn in the icon */
    const char* label;    /* text below the icon */
    uint32_t icon_color;  /* background tint */
    int x, y;             /* computed position */
};

#define NUM_DESKTOP_ICONS 6
static struct desktop_icon desktop_icons[NUM_DESKTOP_ICONS] = {
    { ">_", "Terminal",  0x8022D3EE, 0, 0 },
    { "AI", "AI Lab",    0x8067E8F9, 0, 0 },
    { "Ed", "Editor",    0x804ADE80, 0, 0 },
    { ">_", "Media",     0x80F87171, 0, 0 },
    { "i",  "About",     0x8094A3B8, 0, 0 },
    { "?",  "Help",      0x8067E8F9, 0, 0 },
};

static int desktop_icons_inited = 0;

static void desktop_icons_layout(void) {
    for (int i = 0; i < NUM_DESKTOP_ICONS; i++) {
        desktop_icons[i].x = ICON_AREA_X;
        desktop_icons[i].y = ICON_AREA_Y + i * (ICON_SIZE + ICON_LABEL_H + ICON_GAP);
    }
    desktop_icons_inited = 1;
}

/* Open or focus an app widget */
static void desktop_launch_app(int idx) {
    struct widget* w = NULL;
    switch (idx) {
        case 0: /* Terminal */
            if (!w_terminal) {
                w_terminal = terminal_create(180, 40);
                compositor_add(w_terminal);
            }
            w = w_terminal;
            break;
        case 1: /* AI Lab */
            if (!w_ailab) {
                w_ailab = ailab_create(200, 60);
                compositor_add(w_ailab);
            }
            w = w_ailab;
            break;
        case 2: /* Editor */
            if (!w_editor) {
                w_editor = editor_create(200, 60);
                compositor_add(w_editor);
            }
            w = w_editor;
            break;
        case 3: /* Media */
            if (!w_media) {
                w_media = media_create(250, 100);
                compositor_add(w_media);
            }
            w = w_media;
            break;
        case 4: /* About */
            if (!w_about) {
                w_about = about_create((int)fb_w / 2 - 190, 200);
                compositor_add(w_about);
            }
            w = w_about;
            break;
        case 5: /* Help */
            if (!w_help) {
                w_help = help_create((int)fb_w / 2 - 200, 180);
                compositor_add(w_help);
            }
            w = w_help;
            break;
    }
    if (w) {
        w->visible = 1;
        compositor_bring_to_front(w);
    }
}

/* Render all desktop icons. Called from compositor after background. */
void desktop_icons_render(void) {
    if (!desktop_icons_inited) desktop_icons_layout();

    for (int i = 0; i < NUM_DESKTOP_ICONS; i++) {
        struct desktop_icon* icon = &desktop_icons[i];
        int x = icon->x;
        int y = icon->y;

        /* Icon background: rounded square with tint */
        fb_draw_rounded(x, y, ICON_SIZE, ICON_SIZE, 14,
                        icon->icon_color, 0x4022D3EE);

        /* Glyph: centered, 3x scale */
        int gw = fb_text_width(icon->glyph) * 1;
        int gx = x + (ICON_SIZE - gw * 3) / 2;
        int gy = y + (ICON_SIZE - 48) / 2;
        /* Draw glyph at 3x scale (split chars) */
        for (const char* p = icon->glyph; *p; p++) {
            fb_draw_char_scale(gx, gy, *p, UI_TEXT_PRIMARY, 3);
            gx += 8 * 3;
        }

        /* Label below icon */
        int lw = fb_text_width(icon->label);
        int lx = x + (ICON_SIZE - lw) / 2;
        int ly = y + ICON_SIZE + 2;
        /* Label background for readability */
        fb_fill_rect(lx - 2, ly, lw + 4, ICON_LABEL_H, 0x80050608);
        fb_draw_string(lx, ly, icon->label, UI_TEXT_PRIMARY);
    }
}

/* Check if a click is on a desktop icon. Returns icon index or -1. */
static int desktop_icon_hit(int mx, int my) {
    if (!desktop_icons_inited) desktop_icons_layout();
    for (int i = 0; i < NUM_DESKTOP_ICONS; i++) {
        struct desktop_icon* icon = &desktop_icons[i];
        if (mx >= icon->x && mx < icon->x + ICON_SIZE &&
            my >= icon->y && my < icon->y + ICON_SIZE + ICON_LABEL_H) {
            return i;
        }
    }
    return -1;
}

/* Handle a mouse-down event. Returns 1 if consumed (clicked an icon). */
int desktop_icons_handle_click(int x, int y) {
    int idx = desktop_icon_hit(x, y);
    if (idx >= 0) {
        desktop_launch_app(idx);
        return 1;
    }
    return 0;
}
