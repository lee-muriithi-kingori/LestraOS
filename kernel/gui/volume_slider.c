/*
 * Lestra OS - Volume Slider flyout
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * A small vertical slider popup that appears when the user clicks the
 * volume icon in the top bar. It includes:
 *   - Vertical slider (0..100)
 *   - Mute toggle button
 *   - Numeric value label
 *
 * Writes the chosen volume to the AC97 mixer via ac97_set_master_volume()
 * (implemented in kernel/drivers/audio/ac97.c). If the AC97 controller
 * was not initialised at boot, the function no-ops internally, so this
 * UI remains safe on systems without AC97 audio.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/timer.h>
#include <lestra/printk.h>
#include <string.h>

#define VS_W    56
#define VS_H    220
#define VS_TITLE_H 24
#define VS_SLIDER_H 130

struct vs_state {
    int visible;
    int x, y;          /* anchor (top-left) */
    int volume;        /* 0..100 */
    int muted;
    int dragging;
    uint64_t last_interact_ms;
};

static struct vs_state vs_state;

/* Real implementation lives in kernel/drivers/audio/ac97.c.
 * Declared here as extern (NOT weak) so a missing AC97 driver is a
 * link-time error rather than a silent no-op. The strong definition
 * in ac97.c no-ops cleanly when the controller is absent. */
extern void ac97_set_master_volume(int volume /* 0..100 */);

/* ---------- public API ---------- */
void volume_slider_show_at(int x, int y) {
    vs_state.visible = 1;
    vs_state.x = x;
    vs_state.y = y;
    vs_state.last_interact_ms = timer_get_ms();
    vs_state.dragging = 0;
}

void volume_slider_hide(void) {
    vs_state.visible = 0;
    vs_state.dragging = 0;
}

/* ---------- helpers ---------- */
static int vs_slider_track_x(void) {
    return vs_state.x + VS_W / 2 - 4;
}
static int vs_slider_track_y(void) {
    return vs_state.y + VS_TITLE_H + 16;
}
static int vs_value_to_y(int v) {
    /* v=0 -> bottom of track, v=100 -> top. */
    int top = vs_slider_track_y();
    int h = VS_SLIDER_H;
    return top + h - (v * h) / 100;
}
static int vs_y_to_value(int my) {
    int top = vs_slider_track_y();
    int h = VS_SLIDER_H;
    int v = 100 - ((my - top) * 100) / h;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

static int vs_hit_mute(int mx, int my) {
    int bx = vs_state.x + 4;
    int by = vs_state.y + VS_H - 36;
    return (mx >= bx && mx < bx + VS_W - 8 &&
            my >= by && my < by + 28);
}

/* ---------- render ---------- */
void volume_slider_render(void) {
    if (!vs_state.visible) return;

    /* Auto-hide after 4 s of no interaction (unless dragging). */
    uint64_t now = timer_get_ms();
    if (!vs_state.dragging &&
        now - vs_state.last_interact_ms > 4000) {
        volume_slider_hide();
        return;
    }

    /* Background panel. */
    fb_draw_rounded(vs_state.x, vs_state.y, VS_W, VS_H, 8,
                    0xE60E1422, UI_ACCENT);

    /* Header. */
    fb_draw_string_small(vs_state.x + 6, vs_state.y + 6, "Vol",
                         UI_TEXT_PRIMARY);

    /* Slider track. */
    int tx = vs_slider_track_x();
    int ty = vs_slider_track_y();
    fb_fill_rect(tx, ty, 8, VS_SLIDER_H, 0xFF1E293B);
    /* Fill up to the current value. */
    int vy = vs_value_to_y(vs_state.volume);
    if (vs_state.volume > 0) {
        fb_fill_rect(tx, vy, 8, ty + VS_SLIDER_H - vy,
                     vs_state.muted ? UI_TEXT_FAINT : UI_ACCENT);
    }
    /* Knob. */
    int kx = tx - 8;
    int ky = vy - 8;
    fb_draw_circle(kx + 12, ky + 8, 10,
                   vs_state.muted ? UI_TEXT_FAINT : UI_ACCENT_HOT);

    /* Value label. */
    char buf[8];
    ksnprintf(buf, sizeof(buf), "%u", (unsigned)vs_state.volume);
    int lw = fb_text_width(buf);
    fb_draw_string_small(vs_state.x + (VS_W - lw) / 2,
                         ty + VS_SLIDER_H + 6, buf, UI_TEXT_PRIMARY);

    /* Mute button. */
    int bx = vs_state.x + 4;
    int by = vs_state.y + VS_H - 36;
    uint32_t mbg = vs_state.muted ? UI_DANGER : 0xFF1E293B;
    uint32_t mfg = vs_state.muted ? 0xFF000000 : UI_TEXT_PRIMARY;
    fb_draw_rounded(bx, by, VS_W - 8, 28, 6, mbg, mbg);
    /* Speaker glyph: small trapezoid + arcs. */
    int gx = bx + 8;
    int gy = by + 10;
    fb_fill_rect(gx, gy + 2, 4, 8, mfg);
    fb_fill_rect(gx + 4, gy + 2, 4, 8, mfg);
    if (vs_state.muted) {
        /* Red slash through speaker. */
        fb_draw_line(gx, gy - 1, gx + 16, gy + 13, mfg);
    }
    fb_draw_string_small(bx + 18, by + 8, "Mute", mfg);
}

/* ---------- events ---------- */
int volume_slider_handle_event(struct event* e) {
    if (!vs_state.visible) return 0;
    if (e->type == EV_MOUSE_MOVE) {
        vs_state.last_interact_ms = timer_get_ms();
        if (vs_state.dragging) {
            int v = vs_y_to_value(e->mouse.y);
            vs_state.volume = v;
            if (!vs_state.muted) ac97_set_master_volume(v);
        }
        return 1;  /* consume moves so the icon below doesn't get them */
    }
    if (e->type == EV_MOUSE_DOWN) {
        vs_state.last_interact_ms = timer_get_ms();
        int mx = e->mouse.x, my = e->mouse.y;
        /* Mute button. */
        if (vs_hit_mute(mx, my)) {
            vs_state.muted = !vs_state.muted;
            if (!vs_state.muted) ac97_set_master_volume(vs_state.volume);
            else                 ac97_set_master_volume(0);
            return 1;
        }
        /* Click on track / knob = jump + start drag. */
        int tx = vs_slider_track_x();
        int ty = vs_slider_track_y();
        if (mx >= tx - 12 && mx < tx + 20 &&
            my >= ty - 12 && my < ty + VS_SLIDER_H + 12) {
            vs_state.dragging = 1;
            int v = vs_y_to_value(my);
            vs_state.volume = v;
            if (!vs_state.muted) ac97_set_master_volume(v);
            return 1;
        }
        /* Click outside = dismiss. */
        if (mx < vs_state.x || mx >= vs_state.x + VS_W ||
            my < vs_state.y || my >= vs_state.y + VS_H) {
            volume_slider_hide();
            return 0;  /* let the click fall through */
        }
        return 1;
    }
    if (e->type == EV_MOUSE_UP) {
        vs_state.last_interact_ms = timer_get_ms();
        vs_state.dragging = 0;
        return 1;
    }
    if (e->type == EV_KEY_DOWN && e->key.scancode == 0x01 /* ESC */) {
        volume_slider_hide();
        return 1;
    }
    return 0;
}
