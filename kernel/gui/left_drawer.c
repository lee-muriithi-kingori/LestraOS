/*
 * Lestra OS - Collapsible Left App Drawer
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * A vertical panel on the left side of the screen that contains the app
 * icons. It can be expanded (showing icons + labels) or collapsed
 * (showing just a thin bar). Click the toggle button at the top to
 * expand/collapse. Animation: 200ms slide.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/timer.h>
#include <lestra/printk.h>
#include <string.h>

/* External: icon image data getters + app launchers */
extern const uint8_t* get_icon_data(int idx);
extern struct widget* terminal_create(int x, int y);
extern struct widget* ailab_create(int x, int y);
extern struct widget* about_create(int x, int y);
extern struct widget* help_create(int x, int y);
extern struct widget* editor_create(int x, int y);
extern struct widget* media_create(int x, int y);
extern void compositor_add(struct widget* w);
extern void compositor_bring_to_front(struct widget* w);
extern void compositor_quit(void);

/* Drawer state */
#define DRAWER_W_EXPANDED  80
#define DRAWER_W_COLLAPSED 20
#define DRAWER_ICON_SIZE   44
#define DRAWER_ICON_GAP    12
#define DRAWER_TOP         60
#define DRAWER_TOGGLE_H    32

static int drawer_expanded = 1;     /* start expanded */
static int drawer_anim = 1000;      /* 0..1000, 1000=fully expanded */
static uint64_t drawer_anim_start = 0;
#define DRAWER_ANIM_MS 200

/* App definitions */
struct drawer_app {
    const char* label;
    int icon_idx;
    int y;  /* computed position */
    int hovered;
};

#define NUM_DRAWER_APPS 6
static struct drawer_app drawer_apps[NUM_DRAWER_APPS] = {
    { "Terminal",  0, 0, 0 },
    { "AI Lab",    1, 0, 0 },
    { "Editor",    2, 0, 0 },
    { "Media",     3, 0, 0 },
    { "Files",     4, 0, 0 },
    { "Settings",  5, 0, 0 },
};

static struct widget* drawer_widgets[6] = {0};

static void drawer_launch(int idx) {
    struct widget* w = NULL;
    switch (idx) {
        case 0:
            if (!drawer_widgets[0]) { drawer_widgets[0] = terminal_create(150, 40); compositor_add(drawer_widgets[0]); }
            w = drawer_widgets[0]; break;
        case 1:
            if (!drawer_widgets[1]) { drawer_widgets[1] = ailab_create(170, 60); compositor_add(drawer_widgets[1]); }
            w = drawer_widgets[1]; break;
        case 2:
            if (!drawer_widgets[2]) { drawer_widgets[2] = editor_create(170, 60); compositor_add(drawer_widgets[2]); }
            w = drawer_widgets[2]; break;
        case 3:
            if (!drawer_widgets[3]) { drawer_widgets[3] = media_create(200, 100); compositor_add(drawer_widgets[3]); }
            w = drawer_widgets[3]; break;
        case 5:
            if (!drawer_widgets[5]) { drawer_widgets[5] = about_create((int)fb_w/2-190, 200); compositor_add(drawer_widgets[5]); }
            w = drawer_widgets[5]; break;
    }
    if (w) { w->visible = 1; compositor_bring_to_front(w); }
}

/* Get current drawer width based on animation progress */
static int drawer_get_width(void) {
    if (drawer_anim >= 1000) return DRAWER_W_EXPANDED;
    if (drawer_anim <= 0) return DRAWER_W_COLLAPSED;
    int range = DRAWER_W_EXPANDED - DRAWER_W_COLLAPSED;
    return DRAWER_W_COLLAPSED + (range * drawer_anim) / 1000;
}

void left_drawer_toggle(void) {
    drawer_expanded = !drawer_expanded;
    drawer_anim_start = timer_get_ms();
}

static void drawer_update_anim(void) {
    int target = drawer_expanded ? 1000 : 0;
    if (drawer_anim == target) return;

    uint64_t now = timer_get_ms();
    uint64_t elapsed = now - drawer_anim_start;
    int progress = (int)(elapsed * 1000 / DRAWER_ANIM_MS);
    if (progress >= 1000) progress = 1000;

    if (drawer_expanded) {
        drawer_anim = progress;
    } else {
        drawer_anim = 1000 - progress;
    }
    if (drawer_anim < 0) drawer_anim = 0;
    if (drawer_anim > 1000) drawer_anim = 1000;
}

/* Check if click is on the toggle button */
static int left_drawer_toggle_hit(int x, int y) {
    int w = drawer_get_width();
    return (x >= 0 && x < w && y >= 16 && y < 16 + DRAWER_TOGGLE_H);
}

/* Check if click is on an app icon */
static int drawer_icon_hit(int mx, int my) {
    if (!drawer_expanded && drawer_anim <= 0) return -1;
    int w = drawer_get_width();
    if (mx >= w || mx < 0) return -1;

    for (int i = 0; i < NUM_DRAWER_APPS; i++) {
        int iy = DRAWER_TOP + i * (DRAWER_ICON_SIZE + DRAWER_ICON_GAP);
        if (my >= iy && my < iy + DRAWER_ICON_SIZE) {
            return i;
        }
    }
    return -1;
}

/* Handle mouse events. Returns 1 if consumed. */
int left_drawer_handle_event(struct event* e) {
    if (e->type == EV_MOUSE_MOVE) {
        /* Update hover state */
        for (int i = 0; i < NUM_DRAWER_APPS; i++) {
            drawer_apps[i].hovered = (drawer_icon_hit(e->mouse.x, e->mouse.y) == i);
        }
        return 0;
    }
    if (e->type == EV_MOUSE_DOWN) {
        /* Check toggle button first */
        if (left_drawer_toggle_hit(e->mouse.x, e->mouse.y)) {
            left_drawer_toggle();
            return 1;
        }
        /* Check app icons */
        int idx = drawer_icon_hit(e->mouse.x, e->mouse.y);
        if (idx >= 0) {
            drawer_launch(idx);
            return 1;
        }
    }
    return 0;
}

/* Render the left drawer */
void left_drawer_render(void) {
    drawer_update_anim();
    int dw = drawer_get_width();
    if (dw <= 0) return;

    /* Drawer background panel — subtle, no border line */
    fb_fill_rect(0, 0, dw, (int)fb_h, 0x900D1117);

    /* Toggle button at top — minimal, just >> or << */
    int toggle_y = 16;
    /* No background, just the glyph */
    const char* toggle_glyph = drawer_expanded ? "<<" : ">>";
    int gw = fb_text_width(toggle_glyph);
    int gx = (dw - gw * 2) / 2;
    int gy = toggle_y + (DRAWER_TOGGLE_H - 16) / 2;
    for (const char* p = toggle_glyph; *p; p++) {
        fb_draw_char_scale(gx, gy, *p, UI_ACCENT, 2);
        gx += 16;
    }

    /* App icons */
    if (drawer_anim > 100) {
        for (int i = 0; i < NUM_DRAWER_APPS; i++) {
            int iy = DRAWER_TOP + DRAWER_TOGGLE_H + 16 + i * (DRAWER_ICON_SIZE + DRAWER_ICON_GAP);
            drawer_apps[i].y = iy;

            int icon_size = DRAWER_ICON_SIZE;
            int icon_off = 0;

            /* Hover effect: icon grows slightly */
            if (drawer_apps[i].hovered) {
                icon_size = 52;
                icon_off = -2;
            }

            /* Icon background (rounded tile) */
            uint32_t tile_bg = drawer_apps[i].hovered ? 0x4022D3EE : 0x20121828;
            int tile_x = (dw - icon_size) / 2;
            fb_draw_rounded(tile_x, iy + icon_off, icon_size, icon_size, 10,
                            tile_bg, drawer_apps[i].hovered ? 0x5022D3EE : 0x2022D3EE);

            /* Real icon image */
            const uint8_t* icon_data = get_icon_data(drawer_apps[i].icon_idx);
            if (icon_data) {
                int render_size = icon_size - 8;  /* padding inside tile */
                int icon_x = tile_x + 4;
                int icon_y = iy + icon_off + 4;
                for (int row = 0; row < render_size; row++) {
                    for (int col = 0; col < render_size; col++) {
                        int sx = (col * 48) / render_size;
                        int sy = (row * 48) / render_size;
                        const uint8_t* src = &icon_data[(sy * 48 + sx) * 3];
                        uint8_t r = src[0], g = src[1], b = src[2];
                        if (r < 10 && g < 15 && b < 20) continue;
                        uint32_t color = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                        fb_set_pixel(icon_x + col, icon_y + row, color);
                    }
                }
            }

            /* No labels — minimal design, just icons */
        }
    }

    /* No logo — keep it clean */
}

int left_drawer_get_width(void) {
    return drawer_get_width();
}
