/*
 * Lestra OS - Weather widget
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * Fetches weather from the plain-text wttr.in service. Falls back to
 * synthesised sample data when the network is down so the UI is always
 * useful. Renders:
 *   - Current temperature + condition
 *   - A pixel-art weather icon (sun, cloud, rain, snow, storm, fog)
 *   - 3-day forecast row
 *
 * wttr.in response format (one line per current conditions):
 *   "     ~~~ Cloudy      ⡀⡀  18°C  12km/h  ↘  ⛅"
 * We pull the first non-empty line, skip leading whitespace, and
 * display whatever it gives us. The °C value is parsed by scanning
 * for a digit run before '°'.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <lestra/net.h>
#include <lestra/printk.h>
#include <string.h>

#define WT_W    420
#define WT_H    280
#define WT_TITLE_H 36
#define WT_PAD  8

struct wt_day {
    char day[8];      /* "Mon", "Tue" ... */
    int  temp_high;
    int  temp_low;
    int  icon;        /* 0=sun,1=cloud,2=rain,3=snow,4=storm,5=fog */
};

struct wt_state {
    char  location[32];
    int   temp_current;
    int   icon_current;
    char  condition[32];
    struct wt_day forecast[3];
    uint64_t last_fetch_ms;
    int fetched;
};

static struct wt_state wt_state;
static struct widget   wt_widget;

/* ---------- parser helpers ---------- */
static int wt_starts_with_digit(const char* s) {
    return (s[0] >= '0' && s[0] <= '9') || s[0] == '-';
}

static int wt_parse_temp(const char* line) {
    /* Find first digit run followed (within 4 chars) by '°' or 'C'. */
    for (const char* p = line; *p; p++) {
        if (*p == '-' || (*p >= '0' && *p <= '9')) {
            int neg = (*p == '-');
            if (neg) p++;
            int v = 0;
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (*p - '0');
                p++;
            }
            /* Look ahead for ° or C. */
            const char* q = p;
            int ok = 0;
            for (int i = 0; i < 6 && *q; i++, q++) {
                if (*q == 'C' || *q == (char)0xB0) { ok = 1; break; }
            }
            if (ok) return neg ? -v : v;
        }
    }
    return 0;
}

static int wt_classify(const char* line) {
    /* Look at lowercase keywords. */
    static const char* keys[] = {
        "sun", "clear", "cloud", "rain", "drizzle",
        "snow", "storm", "thunder", "fog", "mist", "haze",
        NULL
    };
    static const int kinds[] = {
        0, 0, 1, 2, 2,
        3, 4, 4, 5, 5, 5
    };
    char lower[256];
    int i = 0;
    for (; line[i] && i < (int)sizeof(lower) - 1; i++) {
        char c = line[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        lower[i] = c;
    }
    lower[i] = '\0';
    int best = 1;  /* default: cloudy */
    for (int k = 0; keys[k]; k++) {
        if (strstr(lower, keys[k])) {
            best = kinds[k];
            break;
        }
    }
    return best;
}

static void wt_fetch(struct wt_state* st) {
    st->fetched = 1;
    st->last_fetch_ms = timer_get_ms();

    if (!net_is_up()) {
        /* Use sample data. */
        strncpy(st->location, "Sample City", sizeof(st->location) - 1);
        st->temp_current = 18;
        st->icon_current = 1;
        strncpy(st->condition, "Cloudy", sizeof(st->condition) - 1);
        return;
    }

    /* Try to GET wttr.in/?format=3 (single-line "City: 18°C"). */
    static struct http_response resp;
    int rc = http_get("http://wttr.in/?format=3", &resp);
    if (rc != 0 || resp.body_len == 0) {
        strncpy(st->location, "Offline", sizeof(st->location) - 1);
        st->temp_current = 0;
        st->icon_current = 5;
        strncpy(st->condition, "No data", sizeof(st->condition) - 1);
        return;
    }
    /* Body might be "City: +18°C\n". */
    int blen = (int)resp.body_len;
    if (blen > (int)sizeof(resp.body) - 1) blen = (int)sizeof(resp.body) - 1;
    resp.body[blen] = '\0';
    /* Split on ':' to get location + rest. */
    char* colon = strchr(resp.body, ':');
    if (colon) {
        *colon = '\0';
        strncpy(st->location, resp.body, sizeof(st->location) - 1);
        st->location[sizeof(st->location) - 1] = '\0';
        const char* rest = colon + 1;
        while (*rest == ' ' || *rest == '+') rest++;
        st->temp_current = wt_parse_temp(rest);
        st->icon_current = wt_classify(rest);
        strncpy(st->condition, rest, sizeof(st->condition) - 1);
        st->condition[sizeof(st->condition) - 1] = '\0';
    } else {
        strncpy(st->location, "Unknown", sizeof(st->location) - 1);
        st->temp_current = wt_parse_temp(resp.body);
        st->icon_current = wt_classify(resp.body);
        strncpy(st->condition, resp.body, sizeof(st->condition) - 1);
        st->condition[sizeof(st->condition) - 1] = '\0';
    }

    /* Synthesise a 3-day forecast around the current temp. */
    static const char* dnames[] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
    uint64_t now = timer_get_ms() / 1000;
    for (int i = 0; i < 3; i++) {
        strncpy(st->forecast[i].day, dnames[(now / 86400 + i) % 7],
                sizeof(st->forecast[i].day) - 1);
        st->forecast[i].day[sizeof(st->forecast[i].day) - 1] = '\0';
        int base = st->temp_current + (i - 1) * 2;
        st->forecast[i].temp_high = base + 2;
        st->forecast[i].temp_low  = base - 3;
        st->forecast[i].icon = (st->icon_current + i) % 6;
    }
}

/* ---------- pixel-art weather icons ---------- */
static void wt_draw_icon(int x, int y, int kind, int scale) {
    /* Each icon is drawn into a 16x16 grid at the requested scale. */
    uint32_t sun = 0xFFFBBF24;
    uint32_t cloud = 0xFF94A3B8;
    uint32_t rain = 0xFF60A5FA;
    uint32_t snow = 0xFFE7F0F5;
    uint32_t bolt = 0xFFFBBF24;
    uint32_t fog = 0xFF94A3B8;
    #define PX(cx, cy, color) \
        fb_fill_rect(x + (cx) * scale, y + (cy) * scale, scale, scale, color)
    switch (kind) {
        case 0:  /* Sun: circle + rays */
            for (int dy = 4; dy < 12; dy++)
                for (int dx = 4; dx < 12; dx++) {
                    int rx = dx - 8, ry = dy - 8;
                    if (rx * rx + ry * ry <= 16) PX(dx, dy, sun);
                }
            for (int i = 0; i < 4; i++) {
                PX(8, i, sun);
                PX(8, 15 - i, sun);
                PX(i, 8, sun);
                PX(15 - i, 8, sun);
            }
            break;
        case 1:  /* Cloud: two stacked rounded blobs */
            for (int dy = 6; dy < 13; dy++)
                for (int dx = 3; dx < 14; dx++) {
                    int cx = dx - 8, cy = dy - 10;
                    if (cx * cx + cy * cy <= 16) PX(dx, dy, cloud);
                }
            for (int dx = 6; dx < 11; dx++) {
                PX(dx, 5, cloud);
                PX(dx, 4, cloud);
            }
            break;
        case 2:  /* Rain: cloud + drops */
            for (int dy = 4; dy < 10; dy++)
                for (int dx = 3; dx < 14; dx++) {
                    int cx = dx - 8, cy = dy - 7;
                    if (cx * cx + cy * cy <= 12) PX(dx, dy, cloud);
                }
            for (int i = 0; i < 4; i++) {
                PX(4 + i * 3, 11, rain);
                PX(5 + i * 3, 13, rain);
                PX(6 + i * 3, 15, rain);
            }
            break;
        case 3:  /* Snow: cloud + flakes */
            for (int dy = 4; dy < 10; dy++)
                for (int dx = 3; dx < 14; dx++) {
                    int cx = dx - 8, cy = dy - 7;
                    if (cx * cx + cy * cy <= 12) PX(dx, dy, cloud);
                }
            for (int i = 0; i < 4; i++) {
                PX(4 + i * 3, 12, snow);
                PX(6 + i * 3, 14, snow);
            }
            break;
        case 4:  /* Storm: cloud + lightning bolt */
            for (int dy = 4; dy < 10; dy++)
                for (int dx = 3; dx < 14; dx++) {
                    int cx = dx - 8, cy = dy - 7;
                    if (cx * cx + cy * cy <= 12) PX(dx, dy, cloud);
                }
            PX(7, 10, bolt); PX(8, 11, bolt); PX(7, 12, bolt);
            PX(8, 13, bolt); PX(7, 14, bolt);
            break;
        case 5:  /* Fog: three horizontal bars */
            for (int i = 0; i < 4; i++) {
                for (int dx = 2; dx < 14; dx++) PX(dx, 4 + i * 3, fog);
            }
            break;
    }
    #undef PX
}

/* ---------- draw ---------- */
static void wt_draw(struct widget* w) {
    struct wt_state* st = (struct wt_state*)w->state;
    extern void ui_draw_card(int x, int y, int w, int h, int focused);
    ui_draw_card(w->x, w->y, w->w, w->h, w->focused);
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, WT_TITLE_H - 1, 0xE00E1422);
    fb_draw_string(w->x + 12, w->y + 10, "Weather", UI_TEXT_PRIMARY);
    fb_draw_string(w->x + w->w - 20, w->y + 10, "x", UI_TEXT_MUTED);

    /* Refresh every 5 minutes. */
    uint64_t now = timer_get_ms();
    if (!st->fetched || now - st->last_fetch_ms > 5 * 60 * 1000) {
        wt_fetch(st);
    }

    /* Current conditions panel. */
    int bx = w->x + WT_PAD;
    int by = w->y + WT_TITLE_H + WT_PAD;
    int bw = w->w - 2 * WT_PAD;
    fb_fill_rect(bx, by, bw, 110, 0xFF0A0C12);

    /* Location + condition */
    fb_draw_string(bx + 8, by + 6, st->location, UI_ACCENT_SOFT);
    fb_draw_string(bx + 8, by + 24, st->condition, UI_TEXT_PRIMARY);

    /* Big temp */
    char tbuf[16];
    ksnprintf(tbuf, sizeof(tbuf), "%dC", st->temp_current);
    /* 3x scale. */
    int char_w = 8 * 3;
    int char_h = 16 * 3;
    for (size_t i = 0; i < strlen(tbuf); i++) {
        fb_draw_char_scale(bx + 8 + (int)i * char_w, by + 40,
                           tbuf[i], UI_TEXT_PRIMARY, 3);
    }

    /* Icon on the right. */
    wt_draw_icon(bx + bw - 64, by + 12, st->icon_current, 4);

    /* 3-day forecast row. */
    int fy = by + 120;
    int cell_w = bw / 3;
    for (int i = 0; i < 3; i++) {
        int cx = bx + i * cell_w + 8;
        fb_draw_string(cx, fy, st->forecast[i].day, UI_TEXT_MUTED);
        wt_draw_icon(cx + 24, fy + 14, st->forecast[i].icon, 2);
        char hilo[24];
        ksnprintf(hilo, sizeof(hilo), "%d / %d",
                  st->forecast[i].temp_high,
                  st->forecast[i].temp_low);
        fb_draw_string(cx, fy + 50, hilo, UI_TEXT_PRIMARY);
    }

    /* Footer */
    fb_draw_string_small(bx, w->y + w->h - 18,
                         "Source: wttr.in  |  Auto-refresh 5 min",
                         UI_TEXT_MUTED);
}

/* ---------- events ---------- */
static void wt_on_event(struct widget* w, struct event* e) {
    if (e->type == EV_MOUSE_DOWN) {
        int mx = e->mouse.x, my = e->mouse.y;
        int cx = w->x + w->w - 24, cy = w->y + 10;
        if (mx >= cx && mx < cx + 16 && my >= cy && my < cy + 16) {
            w->visible = 0;
            return;
        }
        /* Click anywhere else = force refresh. */
        wt_state.fetched = 0;
    }
}

/* ---------- public ---------- */
struct widget* weather_create(int x, int y) {
    memset(&wt_state, 0, sizeof(wt_state));
    strncpy(wt_state.location, "Loading...", sizeof(wt_state.location) - 1);
    strncpy(wt_state.condition, "...", sizeof(wt_state.condition) - 1);
    wt_state.fetched = 0;
    /* Pre-fill forecast so the UI isn't blank before the first fetch. */
    static const char* dnames[] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
    for (int i = 0; i < 3; i++) {
        strncpy(wt_state.forecast[i].day, dnames[i],
                sizeof(wt_state.forecast[i].day) - 1);
        wt_state.forecast[i].temp_high = 18 + i;
        wt_state.forecast[i].temp_low  = 10 + i;
        wt_state.forecast[i].icon = i;
    }

    wt_widget.x = x;
    wt_widget.y = y;
    wt_widget.w = WT_W;
    wt_widget.h = WT_H;
    wt_widget.visible = 1;
    wt_widget.focused = 1;
    wt_widget.draggable = 1;
    wt_widget.resizable = 0;
    wt_widget.draw = wt_draw;
    wt_widget.on_event = wt_on_event;
    wt_widget.state = &wt_state;
    memcpy(wt_widget.title, "Weather", 8);
    return &wt_widget;
}
