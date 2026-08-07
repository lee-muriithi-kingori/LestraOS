/*
 * Lestra OS - Boot Splash Animation
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Three-frame splash sequence rendered to the VESA framebuffer:
 *   Frame A (250 ms): black screen, "lestraOS" types in left-to-right,
 *                     subtitle "by Lee Muriithi Kingori"
 *   Frame B (init):   branding stays, status lines slide in from right,
 *                     progress bar fills 0->100%
 *   Frame C (400 ms): branding brightens, status slides out, fade to black
 *
 * Falls back to no-op if framebuffer is not available.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/font.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <string.h>

/* Splash state - set by kernel_main before calling splash_run */
static const char* splash_status_lines[16];
static int splash_status_count = 0;
static int splash_status_shown = 0;

void splash_set_status(int idx, const char* line) {
    if (idx >= 0 && idx < 16) {
        splash_status_lines[idx] = line;
        if (idx + 1 > splash_status_count) splash_status_count = idx + 1;
    }
}

/* Draw the "lestraOS" wordmark centered horizontally, at given y.
 * Each letter is drawn with a scale factor. */
static void draw_wordmark(int cx, int y, int bright) {
    /* "lestraOS" in a mix of cases for visual identity */
    const char* word = "lestraOS";
    int char_w = 8 * 3;  /* 3x scale = 24px per char */
    int total_w = 0;
    const char* p = word;
    while (*p++) total_w += char_w;
    int x = cx - total_w / 2;
    uint32_t color = bright ? UI_ACCENT_SOFT : UI_ACCENT;
    p = word;
    while (*p) {
        fb_draw_char_scale(x, y, *p, color, 3);
        x += char_w;
        p++;
    }
}

static void draw_subtitle(int cx, int y) {
    const char* sub = "by Lee Muriithi Kingori  -  lestramk.org";
    int w = fb_text_width(sub);
    fb_draw_string(cx - w / 2, y, sub, UI_TEXT_MUTED);
}

/* Frame A: type-in animation */
static void splash_frame_a(void) {
    uint64_t start = timer_get_ms();
    uint64_t duration = 250;
    int cx = (int)fb_w / 2;
    int y = (int)fb_h / 2 - 60;

    while (timer_get_ms() - start < duration) {
        fb_clear(UI_BG_BASE);
        uint64_t elapsed = timer_get_ms() - start;
        int progress = (int)(elapsed * 100 / duration);  /* 0..100 */

        /* Draw wordmark with progressive reveal */
        const char* word = "lestraOS";
        int char_w = 8 * 3;
        int total_w = strlen(word) * char_w;
        int x = cx - total_w / 2;
        int chars_to_show = (progress * (int)strlen(word)) / 100 + 1;
        if (chars_to_show > (int)strlen(word)) chars_to_show = strlen(word);

        for (int i = 0; i < chars_to_show && word[i]; i++) {
            fb_draw_char_scale(x, y, word[i], UI_ACCENT, 3);
            x += char_w;
        }

        /* Subtitle appears at 60% */
        if (progress > 60) {
            draw_subtitle(cx, y + 70);
        }

        fb_swap();
    }
}

/* Frame B: status lines + progress bar */
static void splash_frame_b(void) {
    uint64_t start = timer_get_ms();
    uint64_t duration = 1200;  /* 1.2s for init display */
    int cx = (int)fb_w / 2;
    int wm_y = (int)fb_h / 2 - 120;

    while (timer_get_ms() - start < duration) {
        fb_clear(UI_BG_BASE);
        uint64_t elapsed = timer_get_ms() - start;
        int progress = (int)(elapsed * 100 / duration);

        draw_wordmark(cx, wm_y, 0);
        draw_subtitle(cx, wm_y + 70);

        /* Status panel: bottom-center, 60% wide */
        int panel_w = (int)fb_w * 60 / 100;
        int panel_x = (int)fb_w / 2 - panel_w / 2;
        int panel_y = wm_y + 130;
        int line_h = 20;

        /* Show status lines progressively */
        int lines_to_show = (progress * splash_status_count) / 100;
        if (lines_to_show > splash_status_count) lines_to_show = splash_status_count;

        for (int i = 0; i < lines_to_show && i < splash_status_count; i++) {
            if (splash_status_lines[i]) {
                int slide = 0;
                /* Slide-in animation: 150ms per line */
                int line_progress = progress * splash_status_count / 100 - i;
                if (line_progress < 1) {
                    slide = 12;
                } else {
                    slide = 12 * (1 - line_progress);
                    if (slide < 0) slide = 0;
                }
                uint32_t color = (slide > 0) ? UI_ACCENT_SOFT : UI_TEXT_PRIMARY;
                fb_draw_string(panel_x + slide, panel_y + i * line_h,
                               splash_status_lines[i], color);
            }
        }

        /* Progress bar: 2px tall, 60% wide, accent color */
        int bar_y = panel_y + splash_status_count * line_h + 20;
        int bar_w = panel_w;
        int bar_fill = (bar_w * progress) / 100;
        fb_fill_rect(panel_x, bar_y, bar_w, 2, UI_TEXT_FAINT);
        fb_fill_rect(panel_x, bar_y, bar_fill, 2, UI_ACCENT);

        fb_swap();
    }
}

/* Frame C: brighten + fade out */
static void splash_frame_c(void) {
    uint64_t start = timer_get_ms();
    uint64_t duration = 400;
    int cx = (int)fb_w / 2;
    int wm_y = (int)fb_h / 2 - 60;

    while (timer_get_ms() - start < duration) {
        fb_clear(UI_BG_BASE);
        uint64_t elapsed = timer_get_ms() - start;
        int progress = (int)(elapsed * 100 / duration);

        /* Brighten wordmark (accent -> accent_soft) */
        draw_wordmark(cx, wm_y, progress > 50);

        /* Fade out: overlay black with increasing alpha */
        int alpha = progress * 255 / 100;
        for (uint32_t y = 0; y < fb_h; y += 4) {
            for (uint32_t x = 0; x < fb_w; x += 4) {
                uint32_t px = fb_get_pixel(x, y);
                /* Blend toward black */
                uint8_t r = ((px >> 16) & 0xFF) * (255 - alpha) / 255;
                uint8_t g = ((px >> 8) & 0xFF) * (255 - alpha) / 255;
                uint8_t b = (px & 0xFF) * (255 - alpha) / 255;
                fb_set_pixel(x, y, 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
            }
        }

        fb_swap();
    }
    /* Final clear to black */
    fb_clear(UI_BG_BASE);
    fb_swap();
}

void splash_run(void) {
    if (!fb_available) return;
    pr_info("splash: starting animation\n");
    splash_frame_a();
    splash_frame_b();
    splash_frame_c();
    pr_info("splash: done\n");
}
