/*
 * Lestra OS - Drawer (bottom-sheet app launcher)
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Slides up from the bottom when the FAB is clicked. Contains an app
 * grid that opens cards (Terminal, AI Lab, About, Help) or triggers
 * actions (Reboot, Shutdown).
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <string.h>

/* Drawer state */
static int drawer_open = 0;
static int drawer_anim = 0;  /* 0..1000 (0=closed, 1000=open) */
static uint64_t drawer_anim_start = 0;
#define DRAWER_ANIM_MS 280

/* App tile definitions */
#define MAX_TILES 8
struct app_tile {
    const char* glyph;
    const char* label;
    int x, y, w, h;  /* computed at draw time */
};

static struct app_tile tiles[MAX_TILES] = {
    { ">_",  "Terminal",  0,0,0,0 },
    { "AI",  "AI Lab",    0,0,0,0 },
    { "i",   "About",     0,0,0,0 },
    { "?",   "Help",      0,0,0,0 },
    { "F",   "Files",     0,0,0,0 },
    { "#",   "Settings",  0,0,0,0 },
    { "R",   "Reboot",    0,0,0,0 },
    { "X",   "Shutdown",  0,0,0,0 },
};

#define TILE_SIZE 64
#define TILE_GAP 16
#define GRID_COLS 4

/* External: widget creators */
extern struct widget* terminal_create(int x, int y);
extern struct widget* ailab_create(int x, int y);
extern struct widget* about_create(int x, int y);
extern struct widget* help_create(int x, int y);

/* External: compositor */
extern void compositor_add(struct widget* w);

/* External: reboot/shutdown */
extern void cmd_reboot(void);
extern void cmd_shutdown(void);

/* Track which widgets we've created (avoid duplicates) */
static struct widget* term_widget = NULL;
static struct widget* ailab_w = NULL;
static struct widget* about_w = NULL;
static struct widget* help_w = NULL;

static void drawer_open_app(int idx) {
    switch (idx) {
        case 0: /* Terminal */
            if (!term_widget) {
                term_widget = terminal_create((int)fb_w / 2 - 360, 24);
                compositor_add(term_widget);
            }
            term_widget->visible = 1;
            compositor_bring_to_front(term_widget);
            break;
        case 1: /* AI Lab */
            if (!ailab_w) {
                ailab_w = ailab_create(80, 80);
                compositor_add(ailab_w);
            }
            ailab_w->visible = 1;
            compositor_bring_to_front(ailab_w);
            break;
        case 2: /* About */
            if (!about_w) {
                about_w = about_create((int)fb_w / 2 - 190, 200);
                compositor_add(about_w);
            }
            about_w->visible = 1;
            compositor_bring_to_front(about_w);
            break;
        case 3: /* Help */
            if (!help_w) {
                help_w = help_create((int)fb_w / 2 - 200, 180);
                compositor_add(help_w);
            }
            help_w->visible = 1;
            compositor_bring_to_front(help_w);
            break;
        case 6: /* Reboot */
            drawer_open = 0;
            cmd_reboot();
            break;
        case 7: /* Shutdown */
            drawer_open = 0;
            cmd_shutdown();
            break;
    }
    drawer_open = 0;
}

void drawer_toggle(void) {
    drawer_open = !drawer_open;
    drawer_anim_start = timer_get_ms();
}

/* Check if click is on FAB area (to toggle drawer) */
int fab_contains(int x, int y);  /* defined in compositor.c */

void drawer_handle_fab_click(int x, int y) {
    if (fab_contains(x, y)) {
        drawer_toggle();
    }
}

static int drawer_handle_click(int x, int y) {
    if (!drawer_open && drawer_anim < 100) return 0;
    /* Check if click is on a tile */
    for (int i = 0; i < MAX_TILES; i++) {
        if (x >= tiles[i].x && x < tiles[i].x + tiles[i].w &&
            y >= tiles[i].y && y < tiles[i].y + tiles[i].h) {
            drawer_open_app(i);
            return 1;
        }
    }
    /* Click outside drawer = close */
    drawer_open = 0;
    return 0;
}

void drawer_update_anim(void) {
    int target = drawer_open ? 1000 : 0;
    if (drawer_anim == target) return;

    uint64_t now = timer_get_ms();
    uint64_t elapsed = now - drawer_anim_start;
    int progress = (int)(elapsed * 1000 / DRAWER_ANIM_MS);
    if (progress >= 1000) progress = 1000;

    if (drawer_open) {
        drawer_anim = progress;
    } else {
        drawer_anim = 1000 - progress;
    }
    if (drawer_anim < 0) drawer_anim = 0;
    if (drawer_anim > 1000) drawer_anim = 1000;
}

void drawer_render(void) {
    drawer_update_anim();
    if (drawer_anim <= 0) return;

    int drawer_h = (int)fb_h * 45 / 100;  /* 45% of viewport */
    int drawer_y = (int)fb_h - (drawer_h * drawer_anim / 1000);
    int drawer_w = (int)fb_w;

    /* Drawer background */
    fb_fill_rect(0, drawer_y, drawer_w, drawer_h, 0xE60E1422);
    /* Rounded top corners (approximate) */
    fb_draw_rounded(0, drawer_y, drawer_w, drawer_h, 18,
                    0xE60E1422, 0xE60E1422);

    /* Drag handle */
    int handle_w = 32;
    int handle_x = drawer_w / 2 - handle_w / 2;
    int handle_y = drawer_y + 8;
    fb_fill_rect(handle_x, handle_y, handle_w, 4, UI_TEXT_FAINT);

    /* "Apps" header */
    fb_draw_string(24, drawer_y + 24, "apps", UI_ACCENT);

    /* App grid: 4 columns */
    int grid_y = drawer_y + 50;
    int grid_x_start = 24;
    for (int i = 0; i < MAX_TILES; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;
        tiles[i].x = grid_x_start + col * (TILE_SIZE + TILE_GAP);
        tiles[i].y = grid_y + row * (TILE_SIZE + TILE_GAP + 16);
        tiles[i].w = TILE_SIZE;
        tiles[i].h = TILE_SIZE;

        /* Tile background */
        fb_draw_rounded(tiles[i].x, tiles[i].y, TILE_SIZE, TILE_SIZE, 12,
                        0x80121828, 0x4022D3EE);

        /* Glyph (centered) */
        int gw = fb_text_width(tiles[i].glyph);
        int gx = tiles[i].x + (TILE_SIZE - gw) / 2;
        int gy = tiles[i].y + (TILE_SIZE - 16) / 2;
        fb_draw_string(gx, gy, tiles[i].glyph, UI_ACCENT_SOFT);

        /* Label below tile */
        int lw = fb_text_width(tiles[i].label);
        int lx = tiles[i].x + (TILE_SIZE - lw) / 2;
        fb_draw_string(lx, tiles[i].y + TILE_SIZE + 4, tiles[i].label, UI_TEXT_PRIMARY);
    }
}

int drawer_is_open(void) {
    return drawer_open || drawer_anim > 0;
}

int drawer_handle_event(struct event* e) {
    if (e->type == EV_MOUSE_DOWN) {
        /* Check FAB first */
        if (fab_contains(e->mouse.x, e->mouse.y)) {
            drawer_toggle();
            return 1;
        }
        /* If drawer is open, check tile clicks */
        if (drawer_is_open()) {
            return drawer_handle_click(e->mouse.x, e->mouse.y);
        }
    }
    if (e->type == EV_KEY_DOWN) {
        /* Super key toggles drawer */
        if (e->key.scancode == 0x5B) {  /* LWin */
            drawer_toggle();
            return 1;
        }
        /* Esc closes drawer */
        if (e->key.scancode == 0x01 && drawer_is_open()) {
            drawer_open = 0;
            return 1;
        }
    }
    return 0;
}
