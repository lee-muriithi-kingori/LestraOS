/*
 * Lestra OS - Clock widget
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * A desktop clock with two faces (digital and analog), 12/24h toggle,
 * date, and day-of-week. Click the widget to cycle face; press 'F' to
 * toggle 12/24h format. Analog face is drawn with three hands (hour,
 * minute, second) using fb_draw_line; the math uses a small sin
 * lookup table (no FP, no libm).
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <lestra/rtc.h>
#include <lestra/printk.h>
#include <string.h>

#define CW_W    240
#define CW_H    240
#define CW_TITLE_H 36
#define CW_PAD  8

struct cw_state {
    int analog;     /* 0 = digital, 1 = analog */
    int h24;        /* 1 = 24-hour, 0 = 12-hour */
};

static struct cw_state cw_state;
static struct widget   cw_widget;

/* 64-entry sin table: returns -63..63. */
static const int8_t cw_sin_table[64] = {
     0,  6, 12, 18, 24, 29, 35, 40, 45, 49, 53, 56, 59, 61, 62, 63,
    63, 62, 61, 59, 56, 53, 49, 45, 40, 35, 29, 24, 18, 12,  6,  0,
   -6,-12,-18,-24,-29,-35,-40,-45,-49,-53,-56,-59,-61,-62,-63,-63,
  -63,-62,-61,-59,-56,-53,-49,-45,-40,-35,-29,-24,-18,-12, -6,  0
};
static int cw_sin(int deg) {
    int idx = ((deg % 360) + 360) % 360;
    return cw_sin_table[(idx * 64) / 360];
}
static int cw_cos(int deg) {
    return cw_sin(deg + 90);
}

static const char* cw_dow_name(int y, int m, int d) {
    if (m < 3) { m += 12; y -= 1; }
    int k = y % 100;
    int j = y / 100;
    int h = (d + (13 * (m + 1)) / 5 + k + (k / 4) + (j / 4) + 5 * j) % 7;
    int dow = (h + 6) % 7;
    static const char* days[] = {
        "Sunday","Monday","Tuesday","Wednesday",
        "Thursday","Friday","Saturday"
    };
    return days[dow];
}

static const char* cw_month_name(int m) {
    static const char* names[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    if (m < 1 || m > 12) return "?";
    return names[m - 1];
}

/* ---------- faces ---------- */
static void cw_draw_digital(struct widget* w, uint8_t hh, uint8_t mm,
                            uint8_t ss) {
    int cx = w->x + w->w / 2;
    int cy = w->y + CW_TITLE_H + 60;

    /* Time */
    char buf[16];
    if (cw_state.h24) {
        ksnprintf(buf, sizeof(buf), "%02u:%02u:%02u",
                  (unsigned)hh, (unsigned)mm, (unsigned)ss);
    } else {
        int h12 = hh % 12; if (h12 == 0) h12 = 12;
        const char* ampm = (hh < 12) ? "AM" : "PM";
        ksnprintf(buf, sizeof(buf), "%d:%02u %s",
                  h12, (unsigned)mm, ampm);
    }
    /* 3x scale digits */
    int char_w = 8 * 3;
    int char_h = 16 * 3;
    int tw = (int)strlen(buf) * char_w;
    int tx = cx - tw / 2;
    for (size_t i = 0; i < strlen(buf); i++) {
        fb_draw_char_scale(tx + (int)i * char_w, cy, buf[i],
                           UI_ACCENT_SOFT, 3);
    }

    /* Date below */
    extern void rtc_get_date(uint16_t*, uint8_t*, uint8_t*);
    uint16_t year; uint8_t month, day;
    rtc_get_date(&year, &month, &day);
    char date[48];
    ksnprintf(date, sizeof(date), "%s, %s %u, %u",
              cw_dow_name((int)year, (int)month, (int)day),
              cw_month_name((int)month),
              (unsigned)day, (unsigned)year);
    int dw = fb_text_width(date);
    fb_draw_string(cx - dw / 2, cy + char_h + 20, date, UI_TEXT_PRIMARY);

    /* Format hint */
    const char* hint = cw_state.h24 ? "24-hour  (F to toggle)" :
                                       "12-hour  (F to toggle)";
    int hw = fb_text_width(hint);
    fb_draw_string(cx - hw / 2, cy + char_h + 40, hint, UI_TEXT_MUTED);
}

static void cw_draw_analog(struct widget* w, uint8_t hh, uint8_t mm,
                           uint8_t ss) {
    int cx = w->x + w->w / 2;
    int cy = w->y + CW_TITLE_H + 90;
    int radius = 70;

    /* Outer ring */
    fb_draw_circle(cx, cy, radius + 4, UI_CARD_BORDER);
    fb_draw_circle(cx, cy, radius, UI_ACCENT);

    /* Hour ticks */
    for (int i = 0; i < 12; i++) {
        int deg = i * 30;
        int x1 = cx + (cw_cos(deg) * (radius - 8)) / 63;
        int y1 = cy - (cw_sin(deg) * (radius - 8)) / 63;
        int x2 = cx + (cw_cos(deg) * (radius - 2)) / 63;
        int y2 = cy - (cw_sin(deg) * (radius - 2)) / 63;
        fb_draw_line(x1, y1, x2, y2, UI_TEXT_PRIMARY);
    }
    /* Minute ticks */
    for (int i = 0; i < 60; i++) {
        if (i % 5 == 0) continue;
        int deg = i * 6;
        int x1 = cx + (cw_cos(deg) * (radius - 4)) / 63;
        int y1 = cy - (cw_sin(deg) * (radius - 4)) / 63;
        int x2 = cx + (cw_cos(deg) * (radius - 1)) / 63;
        int y2 = cy - (cw_sin(deg) * (radius - 1)) / 63;
        fb_draw_line(x1, y1, x2, y2, UI_TEXT_MUTED);
    }

    /* Hour hand */
    int hdeg = ((hh % 12) * 30) + (mm * 30) / 60;
    int hx = cx + (cw_cos(hdeg - 90) * (radius - 30)) / 63;
    int hy = cy + (cw_sin(hdeg - 90) * (radius - 30)) / 63;
    fb_draw_line(cx, cy, hx, hy, UI_TEXT_PRIMARY);
    /* Minute hand */
    int mdeg = (mm * 6) + (ss * 6) / 60;
    int mx = cx + (cw_cos(mdeg - 90) * (radius - 14)) / 63;
    int my = cy + (cw_sin(mdeg - 90) * (radius - 14)) / 63;
    fb_draw_line(cx, cy, mx, my, UI_ACCENT);
    /* Second hand */
    int sdeg = ss * 6;
    int sx = cx + (cw_cos(sdeg - 90) * (radius - 6)) / 63;
    int sy = cy + (cw_sin(sdeg - 90) * (radius - 6)) / 63;
    fb_draw_line(cx, cy, sx, sy, UI_DANGER);
    /* Hub */
    fb_draw_circle(cx, cy, 3, UI_ACCENT);

    /* Date in the lower-right of the dial. */
    extern void rtc_get_date(uint16_t*, uint8_t*, uint8_t*);
    uint16_t year; uint8_t month, day;
    rtc_get_date(&year, &month, &day);
    char d[16];
    ksnprintf(d, sizeof(d), "%u %s", (unsigned)day,
              cw_month_name((int)month));
    fb_draw_string_small(cx + 12, cy + 18, d, UI_TEXT_MUTED);
}

/* ---------- main draw ---------- */
static void cw_draw(struct widget* w) {
    extern void ui_draw_card(int x, int y, int w, int h, int focused);
    ui_draw_card(w->x, w->y, w->w, w->h, w->focused);
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, CW_TITLE_H - 1, 0xE00E1422);
    fb_draw_string(w->x + 12, w->y + 10, "Clock", UI_TEXT_PRIMARY);
    fb_draw_string(w->x + w->w - 20, w->y + 10, "x", UI_TEXT_MUTED);

    extern void rtc_get_time(uint8_t*, uint8_t*, uint8_t*);
    uint8_t hh, mm, ss;
    rtc_get_time(&hh, &mm, &ss);

    if (cw_state.analog) cw_draw_analog(w, hh, mm, ss);
    else                 cw_draw_digital(w, hh, mm, ss);

    /* Footer hint */
    const char* hint = "Click: toggle face    F: 12/24h";
    int hw = fb_text_width(hint);
    fb_draw_string_small(w->x + (w->w - hw) / 2, w->y + w->h - 16,
                         hint, UI_TEXT_FAINT);
}

/* ---------- events ---------- */
static void cw_on_event(struct widget* w, struct event* e) {
    if (e->type == EV_MOUSE_DOWN) {
        int mx = e->mouse.x, my = e->mouse.y;
        int cx = w->x + w->w - 24, cy = w->y + 10;
        if (mx >= cx && mx < cx + 16 && my >= cy && my < cy + 16) {
            w->visible = 0;
            return;
        }
        /* Click toggles face. */
        cw_state.analog = !cw_state.analog;
    } else if (e->type == EV_KEY_DOWN) {
        if (e->key.scancode == KEY_F) {
            cw_state.h24 = !cw_state.h24;
        }
    }
}

/* ---------- public ---------- */
struct widget* clock_widget_create(int x, int y) {
    memset(&cw_state, 0, sizeof(cw_state));
    cw_state.analog = 0;
    cw_state.h24 = 1;

    cw_widget.x = x;
    cw_widget.y = y;
    cw_widget.w = CW_W;
    cw_widget.h = CW_H;
    cw_widget.visible = 1;
    cw_widget.focused = 1;
    cw_widget.draggable = 1;
    cw_widget.resizable = 0;
    cw_widget.draw = cw_draw;
    cw_widget.on_event = cw_on_event;
    cw_widget.state = &cw_state;
    memcpy(cw_widget.title, "Clock", 6);
    return &cw_widget;
}
