/*
 * Lestra OS - Brightness flyout
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * A small horizontal slider popup that appears when the user clicks
 * the brightness icon. Range is 10..100% (we never let the screen go
 * fully black). After the compositor finishes rendering the desktop,
 * brightness_apply_post_render() can be called to multiply the whole
 * back buffer by the current brightness — this gives the user a real
 * visible effect even without a real backlight driver.
 *
 * Values are persisted through settings_set_brightness() (ui_pro.h).
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <lestra/ui_pro.h>
#include <lestra/printk.h>
#include <string.h>

#define BR_W    220
#define BR_H    56
#define BR_MIN  10
#define BR_MAX  100

struct br_state {
    int visible;
    int x, y;
    int dragging;
    uint64_t last_interact_ms;
};

static struct br_state br_state;

/* ---------- public API ---------- */
void brightness_show_at(int x, int y) {
    br_state.visible = 1;
    br_state.x = x;
    br_state.y = y;
    br_state.dragging = 0;
    br_state.last_interact_ms = timer_get_ms();
}

void brightness_hide(void) {
    br_state.visible = 0;
    br_state.dragging = 0;
}

/* ---------- helpers ---------- */
static int br_track_x(void) { return br_state.x + 32; }
static int br_track_y(void) { return br_state.y + 24; }
static int br_track_w(void) { return BR_W - 56; }

static int br_value_to_x(int v) {
    int x0 = br_track_x();
    int w  = br_track_w();
    int range = BR_MAX - BR_MIN;
    return x0 + ((v - BR_MIN) * w) / range;
}

static int br_x_to_value(int mx) {
    int x0 = br_track_x();
    int w  = br_track_w();
    int range = BR_MAX - BR_MIN;
    int v = BR_MIN + ((mx - x0) * range) / w;
    if (v < BR_MIN) v = BR_MIN;
    if (v > BR_MAX) v = BR_MAX;
    return v;
}

static void br_set(int v) {
    if (v < BR_MIN) v = BR_MIN;
    if (v > BR_MAX) v = BR_MAX;
    settings_set_brightness(v);
}

/* ---------- render ---------- */
/* Forward decls for the small sin/cos helpers (defined below). */
int sin_a(int deg);
int cos_a(int deg);

void brightness_render(void) {
    if (!br_state.visible) return;

    /* Auto-hide after 4 s. */
    uint64_t now = timer_get_ms();
    if (!br_state.dragging &&
        now - br_state.last_interact_ms > 4000) {
        brightness_hide();
        return;
    }

    /* Background panel. */
    fb_draw_rounded(br_state.x, br_state.y, BR_W, BR_H, 10,
                    0xE60E1422, UI_ACCENT);

    /* Sun icon (left of slider). */
    int ix = br_state.x + 12;
    int iy = br_state.y + 20;
    fb_draw_circle(ix + 6, iy + 6, 4, UI_ACCENT_SOFT);
    for (int i = 0; i < 8; i++) {
        int a = i * 45;
        int dx = (6 * cos_a(a)) / 1000;
        int dy = (6 * sin_a(a)) / 1000;
        fb_draw_line(ix + 6 + dx, iy + 6 + dy,
                     ix + 6 + 2 * dx, iy + 6 + 2 * dy, UI_ACCENT_SOFT);
    }

    /* Track. */
    int tx = br_track_x();
    int ty = br_track_y();
    int tw = br_track_w();
    fb_fill_rect(tx, ty, tw, 6, 0xFF1E293B);
    int v = settings_get_brightness();
    if (v < BR_MIN) v = BR_MIN;
    if (v > BR_MAX) v = BR_MAX;
    int kx = br_value_to_x(v);
    if (v > BR_MIN) {
        int fw = kx - tx;
        fb_fill_rect(tx, ty, fw, 6, UI_ACCENT);
    }
    /* Knob. */
    fb_draw_circle(kx, ty + 3, 8, UI_ACCENT_HOT);

    /* Value label (right of slider). */
    char buf[8];
    ksnprintf(buf, sizeof(buf), "%u%%", (unsigned)v);
    fb_draw_string_small(br_state.x + BR_W - 28, ty - 4, buf,
                         UI_TEXT_PRIMARY);
}

/* Small sin/cos helpers (no libm). 64-entry table, returns -1000..1000. */
static const int16_t br_sin_t[64] = {
      0,  98,  195,  290,  383,  471,  555,  634,
    707,  773,  831,  882,  924,  957,  981,  996,
   1000,  996,  981,  957,  924,  882,  831,  773,
    707,  634,  555,  471,  383,  290,  195,   98,
      0,  -98, -195, -290, -383, -471, -555, -634,
   -707, -773, -831, -882, -924, -957, -981, -996,
  -1000, -996, -981, -957, -924, -882, -831, -773,
   -707, -634, -555, -471, -383, -290, -195,  -98
};

static int br_sin_idx(int deg) {
    int idx = ((deg % 360) + 360) % 360;
    return br_sin_t[(idx * 64) / 360];
}

/* Public sin/cos helpers used inline above. They map deg -> -1000..1000. */
int sin_a(int deg) { return br_sin_idx(deg); }
int cos_a(int deg) { return br_sin_idx(deg + 90); }

/* ---------- events ---------- */
int brightness_handle_event(struct event* e) {
    if (!br_state.visible) return 0;
    if (e->type == EV_MOUSE_MOVE) {
        br_state.last_interact_ms = timer_get_ms();
        if (br_state.dragging) {
            int v = br_x_to_value(e->mouse.x);
            br_set(v);
        }
        return 1;
    }
    if (e->type == EV_MOUSE_DOWN) {
        br_state.last_interact_ms = timer_get_ms();
        int mx = e->mouse.x, my = e->mouse.y;
        /* Click on track? */
        int tx = br_track_x();
        int ty = br_track_y();
        int tw = br_track_w();
        if (mx >= tx - 8 && mx < tx + tw + 8 &&
            my >= ty - 10 && my < ty + 16) {
            br_state.dragging = 1;
            int v = br_x_to_value(mx);
            br_set(v);
            return 1;
        }
        /* Click outside = dismiss. */
        if (mx < br_state.x || mx >= br_state.x + BR_W ||
            my < br_state.y || my >= br_state.y + BR_H) {
            brightness_hide();
            return 0;
        }
        return 1;
    }
    if (e->type == EV_MOUSE_UP) {
        br_state.last_interact_ms = timer_get_ms();
        br_state.dragging = 0;
        return 1;
    }
    if (e->type == EV_KEY_DOWN && e->key.scancode == 0x01 /* ESC */) {
        brightness_hide();
        return 1;
    }
    return 0;
}

/* ---------- post-render dimming ---------- */
void brightness_apply_post_render(void) {
    /* Multiply every pixel in fb_back by (brightness/100) so the user
     * actually sees a dimmer screen. Called by the compositor after
     * everything else is drawn, before fb_swap(). */
    int v = settings_get_brightness();
    if (v >= BR_MAX) return;       /* no dimming at 100% */
    if (v < BR_MIN) v = BR_MIN;
    /* Fixed-point: scale by v/100. */
    uint32_t scale = (uint32_t)v;
    if (!fb_back) return;
    for (uint32_t y = 0; y < fb_h; y++) {
        uint32_t* row = &fb_back[y * fb_w];
        for (uint32_t x = 0; x < fb_w; x++) {
            uint32_t c = row[x];
            uint8_t r = (uint8_t)(((c >> 16) & 0xFF) * scale / 100);
            uint8_t g = (uint8_t)(((c >> 8)  & 0xFF) * scale / 100);
            uint8_t b = (uint8_t)((c & 0xFF) * scale / 100);
            row[x] = 0xFF000000u |
                     ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | b;
        }
    }
}
