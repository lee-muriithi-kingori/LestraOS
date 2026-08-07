/*
 * Lestra OS - Wallpaper picker
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Six gradient presets the user can pick from. The choice is
 * persisted to /etc/wallpaper as a single byte (0..5). The compositor
 * can call wallpaper_render() each frame to paint the active gradient
 * (this replaces the static gradient in compositor.c if desired).
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/vfs.h>
#include <lestra/printk.h>
#include <string.h>

#define WALLPAPER_PATH "/etc/wallpaper"

#define WALL_COUNT 6

struct wallpaper_preset {
    const char* name;
    uint32_t top;       /* gradient top color */
    uint32_t bottom;    /* gradient bottom color */
    uint32_t accent;    /* faint accent for vignette */
};

static const struct wallpaper_preset wall_presets[WALL_COUNT] = {
    /* 0: Cyan Night (default Lestra look) */
    { "Cyan Night",   0xFF0E1422, 0xFF050608, 0xFF22D3EE },
    /* 1: Aurora */
    { "Aurora",       0xFF0F2A1D, 0xFF03121A, 0xFF4ADE80 },
    /* 2: Sunset */
    { "Sunset",       0xFF3B1F4F, 0xFF1A0A2C, 0xFFFBBF24 },
    /* 3: Ocean */
    { "Ocean",        0xFF0A2540, 0xFF021024, 0xFF60A5FA },
    /* 4: Rose */
    { "Rose",         0xFF3F0D23, 0xFF1A0510, 0xFFEC4899 },
    /* 5: Mono */
    { "Mono",         0xFF1A1A1A, 0xFF050505, 0xFF94A3B8 },
};

struct wp_state {
    int current;
    int inited;
    /* Picker window */
    struct widget picker_widget;
    int picker_open;
};

static struct wp_state wp_state;

/* ---------- helpers ---------- */
static void wp_load(void) {
    wp_state.current = 0;
    wp_state.inited = 1;
    int fd = vfs_open(WALLPAPER_PATH, O_RDONLY);
    if (fd < 0) return;
    char buf[8];
    ssize_t n = vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    int v = 0;
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            v = buf[i] - '0';
            break;
        }
    }
    if (v >= 0 && v < WALL_COUNT) wp_state.current = v;
}

static void wp_save(void) {
    int fd = vfs_open(WALLPAPER_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    char buf[4];
    int n = ksnprintf(buf, sizeof(buf), "%d\n", wp_state.current);
    vfs_write(fd, buf, n);
    vfs_close(fd);
}

/* ---------- public API ---------- */
void wallpaper_set(int idx) {
    if (!wp_state.inited) wp_load();
    if (idx < 0 || idx >= WALL_COUNT) return;
    wp_state.current = idx;
    wp_save();
    pr_info("wallpaper: set to '%s'\n", wall_presets[idx].name);
}

int wallpaper_get(void) {
    if (!wp_state.inited) wp_load();
    return wp_state.current;
}

void wallpaper_render(void) {
    if (!wp_state.inited) wp_load();
    const struct wallpaper_preset* p = &wall_presets[wp_state.current];
    /* Vertical gradient top -> bottom. */
    uint8_t tr = (uint8_t)((p->top >> 16) & 0xFF);
    uint8_t tg = (uint8_t)((p->top >> 8) & 0xFF);
    uint8_t tb = (uint8_t)(p->top & 0xFF);
    uint8_t br = (uint8_t)((p->bottom >> 16) & 0xFF);
    uint8_t bg = (uint8_t)((p->bottom >> 8) & 0xFF);
    uint8_t bb = (uint8_t)(p->bottom & 0xFF);
    for (uint32_t y = 0; y < fb_h; y++) {
        uint32_t t = y * 256 / fb_h;
        uint8_t r = (uint8_t)((tr * (256 - t) + br * t) / 256);
        uint8_t g = (uint8_t)((tg * (256 - t) + bg * t) / 256);
        uint8_t b = (uint8_t)((tb * (256 - t) + bb * t) / 256);
        uint32_t* row = &fb_back[y * fb_w];
        uint32_t color = 0xFF000000u |
                         ((uint32_t)r << 16) |
                         ((uint32_t)g << 8) | b;
        for (uint32_t x = 0; x < fb_w; x++) row[x] = color;
    }
    /* Faint radial vignette using the accent color. */
    int cx = (int)fb_w / 2;
    int cy = (int)fb_h / 2;
    int maxd = cx > cy ? cx : cy;
    for (int y = 0; y < (int)fb_h; y += 4) {
        for (int x = 0; x < (int)fb_w; x += 4) {
            int dx = x - cx, dy = y - cy;
            int d = dx * dx + dy * dy;
            if (d > maxd * maxd) continue;
            int alpha = (d * 24) / (maxd * maxd);
            uint32_t src = ((uint32_t)alpha << 24) | (p->accent & 0xFFFFFFu);
            uint32_t cur = fb_get_pixel(x, y);
            fb_set_pixel(x, y, fb_blend(cur, src));
        }
    }
}

/* ---------- picker window ---------- */
#define WP_W    480
#define WP_H    320
#define WP_TITLE_H 36
#define WP_PAD  8
#define WP_CELL_W  140
#define WP_CELL_H  100

static void wp_picker_draw(struct widget* w) {
    extern void ui_draw_card(int x, int y, int w, int h, int focused);
    ui_draw_card(w->x, w->y, w->w, w->h, w->focused);
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, WP_TITLE_H - 1, 0xE00E1422);
    fb_draw_string(w->x + 12, w->y + 10, "Wallpaper", UI_TEXT_PRIMARY);
    fb_draw_string(w->x + w->w - 20, w->y + 10, "x", UI_TEXT_MUTED);

    int bx = w->x + WP_PAD;
    int by = w->y + WP_TITLE_H + WP_PAD;
    int cols = (w->w - 2 * WP_PAD) / WP_CELL_W;
    if (cols < 1) cols = 1;
    for (int i = 0; i < WALL_COUNT; i++) {
        int cx = bx + (i % cols) * WP_CELL_W + 4;
        int cy = by + (i / cols) * WP_CELL_H + 4;
        int cw = WP_CELL_W - 8;
        int ch = WP_CELL_H - 24;
        /* Gradient thumbnail. */
        const struct wallpaper_preset* p = &wall_presets[i];
        uint8_t tr = (uint8_t)((p->top >> 16) & 0xFF);
        uint8_t tg = (uint8_t)((p->top >> 8) & 0xFF);
        uint8_t tb = (uint8_t)(p->top & 0xFF);
        uint8_t br = (uint8_t)((p->bottom >> 16) & 0xFF);
        uint8_t bg = (uint8_t)((p->bottom >> 8) & 0xFF);
        uint8_t bb = (uint8_t)(p->bottom & 0xFF);
        for (int y = 0; y < ch; y++) {
            int t = y * 256 / ch;
            uint8_t r = (uint8_t)((tr * (256 - t) + br * t) / 256);
            uint8_t g = (uint8_t)((tg * (256 - t) + bg * t) / 256);
            uint8_t b = (uint8_t)((tb * (256 - t) + bb * t) / 256);
            uint32_t c = 0xFF000000u |
                         ((uint32_t)r << 16) |
                         ((uint32_t)g << 8) | b;
            fb_fill_rect(cx, cy + y, cw, 1, c);
        }
        fb_draw_rect(cx, cy, cw, ch,
                     (i == wp_state.current) ? UI_ACCENT : UI_CARD_BORDER);
        /* Label below. */
        int lw = fb_text_width(p->name);
        fb_draw_string_small(cx + (cw - lw) / 2, cy + ch + 4, p->name,
                             UI_TEXT_PRIMARY);
    }
    /* Footer hint. */
    fb_draw_string_small(w->x + WP_PAD, w->y + w->h - 16,
                         "Click a thumbnail to set as wallpaper.",
                         UI_TEXT_MUTED);
}

static void wp_picker_on_event(struct widget* w, struct event* e) {
    if (e->type != EV_MOUSE_DOWN) return;
    int mx = e->mouse.x, my = e->mouse.y;
    int cx = w->x + w->w - 24, cy = w->y + 10;
    if (mx >= cx && mx < cx + 16 && my >= cy && my < cy + 16) {
        w->visible = 0;
        wp_state.picker_open = 0;
        return;
    }
    int bx = w->x + WP_PAD;
    int by = w->y + WP_TITLE_H + WP_PAD;
    int cols = (w->w - 2 * WP_PAD) / WP_CELL_W;
    if (cols < 1) cols = 1;
    for (int i = 0; i < WALL_COUNT; i++) {
        int ax = bx + (i % cols) * WP_CELL_W + 4;
        int ay = by + (i / cols) * WP_CELL_H + 4;
        int aw = WP_CELL_W - 8;
        int ah = WP_CELL_H - 8;
        if (mx >= ax && mx < ax + aw && my >= ay && my < ay + ah) {
            wallpaper_set(i);
            return;
        }
    }
}

struct widget* wallpaper_picker_create(int x, int y) {
    if (!wp_state.inited) wp_load();
    wp_state.picker_open = 1;
    wp_state.picker_widget.x = x;
    wp_state.picker_widget.y = y;
    wp_state.picker_widget.w = WP_W;
    wp_state.picker_widget.h = WP_H;
    wp_state.picker_widget.visible = 1;
    wp_state.picker_widget.focused = 1;
    wp_state.picker_widget.draggable = 1;
    wp_state.picker_widget.resizable = 0;
    wp_state.picker_widget.draw = wp_picker_draw;
    wp_state.picker_widget.on_event = wp_picker_on_event;
    wp_state.picker_widget.state = NULL;
    memcpy(wp_state.picker_widget.title, "Wallpaper", 10);
    return &wp_state.picker_widget;
}
