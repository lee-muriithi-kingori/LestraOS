/*
 * Lestra OS - Toast notification system
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Slide-in toast notifications that appear at the top-right of the
 * screen, stack vertically, auto-dismiss after a timeout, and can be
 * dismissed manually by clicking.
 *
 * Each notification has:
 *   - title (bold)
 *   - body (muted)
 *   - accent color (left border, also tint the icon background)
 *   - duration_ms (0 = sticky; never auto-dismisses)
 *
 * Style: glass bubbles (UI_CARD_BG + UI_ACCENT border) with a colored
 * 3px left border so users can identify the notification type at a
 * glance (cyan = info, green = success, red = error, amber = warning).
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/font.h>
#include <lestra/timer.h>
#include <lestra/printk.h>
#include <string.h>

#define NOTIFY_MAX         8
#define NOTIFY_W         300
#define NOTIFY_H          72
#define NOTIFY_GAP          8
#define NOTIFY_MARGIN_X    16
#define NOTIFY_MARGIN_Y    56
#define NOTIFY_DURATION  4500
#define NOTIFY_SLIDE_MS   250

struct notification {
    int      used;
    char     title[40];
    char     body[80];
    uint32_t color;          /* accent color (0xAARRGGBB) */
    uint64_t shown_ms;
    uint64_t duration_ms;    /* 0 = sticky */
    int      slide_in;       /* 0..1 fraction of slide-in completed */
    int      slide_offset;   /* px off the right edge (negative = visible) */
};

static struct notification notifs[NOTIFY_MAX];
static uint64_t last_tick_ms = 0;

int notify_count(void) {
    int n = 0;
    for (int i = 0; i < NOTIFY_MAX; i++) if (notifs[i].used) n++;
    return n;
}

void notify_dismiss_all(void) {
    memset(notifs, 0, sizeof(notifs));
}

void notify_show(const char* title, const char* body, uint32_t color) {
    /* Find a free slot. If none, evict the oldest. */
    int slot = -1;
    uint64_t oldest = (uint64_t)-1;
    int oldest_idx = 0;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!notifs[i].used) { slot = i; break; }
        if (notifs[i].shown_ms < oldest) {
            oldest = notifs[i].shown_ms;
            oldest_idx = i;
        }
    }
    if (slot < 0) slot = oldest_idx;
    memset(&notifs[slot], 0, sizeof(notifs[slot]));
    notifs[slot].used = 1;
    notifs[slot].color = color ? color : UI_ACCENT;
    notifs[slot].shown_ms = timer_get_ms();
    notifs[slot].duration_ms = NOTIFY_DURATION;
    notifs[slot].slide_in = 0;
    notifs[slot].slide_offset = NOTIFY_W + NOTIFY_MARGIN_X;
    if (title) {
        strncpy(notifs[slot].title, title, sizeof(notifs[slot].title) - 1);
        notifs[slot].title[sizeof(notifs[slot].title) - 1] = '\0';
    }
    if (body) {
        strncpy(notifs[slot].body, body, sizeof(notifs[slot].body) - 1);
        notifs[slot].body[sizeof(notifs[slot].body) - 1] = '\0';
    }
}

static int ease_out_cubic(int t01) {
    /* (1 - (1-t)^3) * 1000 / 1000, in integer math. t01 is 0..1000. */
    if (t01 < 0) t01 = 0;
    if (t01 > 1000) t01 = 1000;
    int inv = 1000 - t01;
    int inv3 = (inv * inv / 1000) * inv / 1000;
    return 1000 - inv3;
}

void notify_render(void) {
    uint64_t now = timer_get_ms();
    uint32_t dt = (uint32_t)(now - last_tick_ms);
    last_tick_ms = now;

    /* Lay out from top-right downward. */
    int x_anchor = (int)fb_w - NOTIFY_W - NOTIFY_MARGIN_X;
    int y = NOTIFY_MARGIN_Y;

    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!notifs[i].used) continue;
        struct notification* n = &notifs[i];

        /* Advance slide-in. */
        if (n->slide_in < 1000) {
            int inc = (int)((uint64_t)dt * 1000 / NOTIFY_SLIDE_MS);
            n->slide_in += inc;
            if (n->slide_in > 1000) n->slide_in = 1000;
        }
        int eased = ease_out_cubic(n->slide_in);
        n->slide_offset = ((1000 - eased) * (NOTIFY_W + NOTIFY_MARGIN_X)) / 1000;

        /* Auto-dismiss. */
        if (n->duration_ms > 0 && (now - n->shown_ms) > n->duration_ms) {
            n->used = 0;
            continue;
        }

        int bx = x_anchor + n->slide_offset;

        /* Drop shadow. */
        fb_draw_rounded(bx + 2, y + 4, NOTIFY_W, NOTIFY_H, 10,
                        0x55000000u, 0x55000000u);
        /* Glass body. */
        fb_draw_rounded(bx, y, NOTIFY_W, NOTIFY_H, 10,
                        UI_CARD_BG, UI_CARD_BORDER);
        /* Colored left accent border (3 px wide). */
        fb_fill_rect(bx, y + 4, 3, NOTIFY_H - 8, n->color);
        /* Title. */
        fb_draw_string(bx + 14, y + 10, n->title, UI_TEXT_PRIMARY);
        /* Body. */
        fb_draw_string_small(bx + 14, y + 32, n->body, UI_TEXT_MUTED);
        /* Small dismiss "x" in the top-right. */
        fb_draw_string(bx + NOTIFY_W - 18, y + 8, "x", UI_TEXT_FAINT);

        y += NOTIFY_H + NOTIFY_GAP;
    }
}

int notify_handle_event(struct event* e) {
    if (!e) return 0;
    if (e->type != EV_MOUSE_DOWN) return 0;

    int mx = e->mouse.x;
    int my = e->mouse.y;
    int x_anchor = (int)fb_w - NOTIFY_W - NOTIFY_MARGIN_X;
    int y = NOTIFY_MARGIN_Y;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!notifs[i].used) continue;
        int bx = x_anchor + notifs[i].slide_offset;
        if (mx >= bx && mx < bx + NOTIFY_W &&
            my >= y && my < y + NOTIFY_H) {
            notifs[i].used = 0;  /* click-to-dismiss */
            return 1;
        }
        y += NOTIFY_H + NOTIFY_GAP;
    }
    return 0;
}
