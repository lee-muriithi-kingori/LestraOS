/*
 * Lestra OS - Lock Screen
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Full-screen overlay shown when the user locks the session. Renders a
 * dimmed/blurred snapshot of the desktop behind it (we approximate the
 * blur with a translucent dark wash because we don't have a real
 * two-pass Gaussian in the kernel), a large clock centred on screen,
 * the date, and a "Press any key to unlock" hint.
 *
 * The lock screen is a singleton overlay: lock_screen_show() raises it,
 * lock_screen_hide() drops it, and any input event dismisses it (unless
 * a password is required — for now we keep it click/key to dismiss so
 * the OS is usable without a password backend).
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

/* ----- state ----- */
static int lock_active = 0;
static uint64_t lock_shown_ms = 0;

/* ----- helpers ----- */
static const char* month_name(int m) {
    static const char* names[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    if (m < 1 || m > 12) return "?";
    return names[m - 1];
}

static const char* dow_name(int y, int m, int d) {
    /* Zeller's congruence: 0=Saturday..6=Friday, convert to 0=Sunday */
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

static void lock_blur_background(void) {
    /* Approximate a blur+dim by overlaying a translucent dark wash and
     * downscaling neighbour pixels by 50%. This is cheap and looks
     * good enough for a lock screen backdrop. */
    if (!fb_back) return;
    /* First, a strong dark wash so the lock screen reads as "dimmed". */
    for (uint32_t y = 0; y < fb_h; y++) {
        uint32_t* row = &fb_back[y * fb_w];
        for (uint32_t x = 0; x < fb_w; x++) {
            uint32_t c = row[x];
            /* Halve each channel and add 0x05/0x06/0x08 to bias toward navy. */
            uint8_t r = (uint8_t)(((c >> 16) & 0xFF) >> 1) + 0x05;
            uint8_t g = (uint8_t)(((c >> 8)  & 0xFF) >> 1) + 0x06;
            uint8_t b = (uint8_t)((c & 0xFF) >> 1) + 0x08;
            row[x] = 0xFF000000u | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | b;
        }
    }
}

static void lock_draw_big_clock(void) {
    extern void rtc_get_time(uint8_t*, uint8_t*, uint8_t*);
    extern void rtc_get_date(uint16_t*, uint8_t*, uint8_t*);
    uint8_t hh, mm, ss;
    uint16_t year;
    uint8_t month, day;
    rtc_get_time(&hh, &mm, &ss);
    rtc_get_date(&year, &month, &day);

    char time_buf[16];
    ksnprintf(time_buf, sizeof(time_buf), "%u:%02u", (unsigned)hh,
              (unsigned)mm);

    /* Big clock — 4x scaled font, centred horizontally. */
    int char_w = 8 * 4;
    int char_h = 16 * 4;
    int text_w = (int)strlen(time_buf) * char_w;
    int cx = ((int)fb_w - text_w) / 2;
    int cy = ((int)fb_h - char_h) / 2 - 20;

    /* Subtle glow behind the digits. */
    for (int r = 16; r > 0; r -= 2) {
        uint32_t glow = 0x1022D3EEu | ((uint32_t)(r * 2) << 24);
        for (int i = 0; i < (int)strlen(time_buf); i++) {
            int gx = cx + i * char_w;
            fb_fill_rect(gx - r, cy - r, char_w + 2 * r, char_h + 2 * r,
                         glow);
        }
    }

    /* The digits themselves. */
    for (size_t i = 0; i < strlen(time_buf); i++) {
        fb_draw_char_scale(cx + (int)i * char_w, cy, time_buf[i],
                           UI_ACCENT_SOFT, 4);
    }

    /* Date + day of week beneath. */
    char date_buf[48];
    ksnprintf(date_buf, sizeof(date_buf), "%s, %s %u, %u",
              dow_name((int)year, (int)month, (int)day),
              month_name((int)month), (unsigned)day, (unsigned)year);
    int dw = fb_text_width(date_buf);
    fb_draw_string(((int)fb_w - dw) / 2, cy + char_h + 24, date_buf,
                   UI_TEXT_PRIMARY);

    /* "Press any key to unlock" hint (blinks). */
    uint64_t now = timer_get_ms();
    if (((now - lock_shown_ms) / 600) % 2 == 0) {
        const char* hint = "Press any key to unlock";
        int hw = fb_text_width(hint);
        fb_draw_string(((int)fb_w - hw) / 2, cy + char_h + 60, hint,
                       UI_TEXT_MUTED);
    }

    /* Small lock glyph centred at the top of the screen. */
    int lx = ((int)fb_w) / 2 - 12;
    int ly = cy - 80;
    /* Padlock body (square) */
    fb_draw_rounded(lx, ly + 12, 24, 18, 3, UI_ACCENT, UI_ACCENT);
    /* Shackle (arc approximated by two side strokes + top stroke) */
    fb_draw_line(lx + 5, ly + 12, lx + 5, ly + 6, UI_ACCENT);
    fb_draw_line(lx + 19, ly + 12, lx + 19, ly + 6, UI_ACCENT);
    fb_draw_line(lx + 5, ly + 6, lx + 19, ly + 6, UI_ACCENT);
    /* Keyhole */
    fb_fill_rect(lx + 11, ly + 18, 2, 4, 0xFF0A0C12);
}

/* ----- public API ----- */
void lock_screen_show(void) {
    if (lock_active) return;
    lock_active = 1;
    lock_shown_ms = timer_get_ms();
    pr_info("lock_screen: session locked\n");
}

void lock_screen_hide(void) {
    if (!lock_active) return;
    lock_active = 0;
    pr_info("lock_screen: session unlocked\n");
}

void lock_screen_toggle(void) {
    if (lock_active) lock_screen_hide();
    else             lock_screen_show();
}

int lock_screen_is_active(void) {
    return lock_active;
}

void lock_screen_render(void) {
    if (!lock_active) return;
    /* Blur/dim the desktop behind us. */
    lock_blur_background();

    /* Translucent dark vignette so the clock reads better. */
    fb_fill_rect(0, 0, (int)fb_w, (int)fb_h, 0x80050608u);

    /* Big centred clock. */
    lock_draw_big_clock();

    /* Branding at the bottom. */
    const char* brand = "Lestra OS";
    int bw = fb_text_width(brand);
    fb_draw_string(((int)fb_w - bw) / 2, (int)fb_h - 32, brand,
                   UI_TEXT_FAINT);
}

int lock_screen_handle_event(struct event* e) {
    if (!lock_active) return 0;
    /* Any key or mouse click dismisses the lock screen. */
    if (e->type == EV_KEY_DOWN || e->type == EV_MOUSE_DOWN) {
        lock_screen_hide();
        return 1;  /* consumed */
    }
    /* Swallow mouse moves too so they don't reach windows behind us. */
    if (e->type == EV_MOUSE_MOVE || e->type == EV_MOUSE_UP ||
        e->type == EV_KEY_UP) {
        return 1;
    }
    return 0;
}
