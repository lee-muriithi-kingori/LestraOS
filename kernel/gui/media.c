/*
 * Lestra OS - Media Player card
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * This is a media player UI. It has the visual interface (play/pause/stop
 * buttons, progress bar, volume control) but CANNOT actually play media.
 *
 * Why no playback:
 *   - Video requires codec implementations (H.264, MPEG, VP8, etc.) which
 *     are each thousands of lines of code and often patent-encumbered.
 *   - Audio requires a sound card driver (we have none — no AC97/HDA).
 *   - Container parsing (MP4, MKV, AVI) is another large sub-project.
 *
 * This card exists so the desktop has a media player icon, but clicking
 * play shows an honest "no codecs available" message. A real media player
 * would need a userspace with codec libraries (like libavcodec).
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <string.h>

#define MEDIA_W  480
#define MEDIA_H  320
#define MEDIA_TITLE_H 36

struct media_state {
    int active;
    int playing;
    int progress;  /* 0..1000 */
    char track_name[64];
};

static struct media_state media_state;
static struct widget media_widget;

static void media_draw(struct widget* w);
static void media_on_event(struct widget* w, struct event* e);

static void media_draw(struct widget* w) {
    struct media_state* st = (struct media_state*)w->state;

    fb_draw_rounded(w->x, w->y, w->w, w->h, 14,
                    UI_CARD_BG, st->active ? UI_ACCENT : UI_CARD_BORDER);
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, MEDIA_TITLE_H - 1, 0xE00E1422);
    fb_draw_string(w->x + 12, w->y + 10, "Media Player", UI_TEXT_PRIMARY);
    fb_draw_string(w->x + w->w - 20, w->y + 10, "x", UI_TEXT_MUTED);

    int body_x = w->x + 16;
    int body_y = w->y + MEDIA_TITLE_H + 16;

    /* Video preview area (black box) */
    int vid_w = w->w - 32;
    int vid_h = 140;
    fb_fill_rect(body_x, body_y, vid_w, vid_h, 0xFF000000);
    fb_draw_rect(body_x, body_y, vid_w, vid_h, UI_CARD_BORDER);

    /* "No signal" pattern */
    uint64_t now = timer_get_ms();
    for (int y = 0; y < vid_h; y += 2) {
        if (((y + (int)(now / 50)) % 8) < 4) {
            fb_fill_rect(body_x, body_y + y, vid_w, 1, 0xFF0A0A0A);
        }
    }

    /* Center text */
    const char* msg = st->playing
        ? "No codecs available"
        : "No media loaded";
    int mw = fb_text_width(msg);
    fb_draw_string(body_x + (vid_w - mw) / 2, body_y + vid_h / 2 - 8,
                   msg, UI_TEXT_MUTED);

    /* Track name */
    fb_draw_string(body_x, body_y + vid_h + 12, st->track_name, UI_TEXT_PRIMARY);

    /* Progress bar */
    int bar_y = body_y + vid_h + 36;
    int bar_w = vid_w;
    fb_fill_rect(body_x, bar_y, bar_w, 4, UI_TEXT_FAINT);
    int fill_w = (bar_w * st->progress) / 1000;
    fb_fill_rect(body_x, bar_y, fill_w, 4, UI_ACCENT);

    /* Control buttons */
    int btn_y = bar_y + 16;
    int btn_size = 32;
    int btn_gap = 12;
    int total_btns = 3;  /* prev, play/stop, next */
    int total_w = btn_size * total_btns + btn_gap * (total_btns - 1);
    int btn_x = body_x + (vid_w - total_w) / 2;

    /* Prev button */
    fb_draw_rounded(btn_x, btn_y, btn_size, btn_size, 8,
                    0x80121828, UI_CARD_BORDER);
    fb_draw_string(btn_x + 10, btn_y + 8, "|<", UI_TEXT_PRIMARY);

    /* Play/Stop button */
    btn_x += btn_size + btn_gap;
    fb_draw_rounded(btn_x, btn_y, btn_size, btn_size, 8,
                    st->playing ? 0x80F87171 : 0x8022D3EE, UI_CARD_BORDER);
    fb_draw_string(btn_x + 10, btn_y + 8, st->playing ? "[]" : ">", UI_TEXT_PRIMARY);

    /* Next button */
    btn_x += btn_size + btn_gap;
    fb_draw_rounded(btn_x, btn_y, btn_size, btn_size, 8,
                    0x80121828, UI_CARD_BORDER);
    fb_draw_string(btn_x + 10, btn_y + 8, ">|", UI_TEXT_PRIMARY);

    /* Info text at bottom */
    int info_y = w->y + w->h - 24;
    fb_draw_string(body_x, info_y,
                   "Video/audio codecs not implemented (needs libavcodec + sound driver)",
                   UI_TEXT_FAINT);
}

static void media_on_event(struct widget* w, struct event* e) {
    struct media_state* st = (struct media_state*)w->state;

    if (e->type == EV_MOUSE_DOWN) {
        st->active = 1;

        /* Check play/stop button */
        int body_x = w->x + 16;
        int body_y = w->y + MEDIA_TITLE_H + 16;
        int vid_h = 140;
        int bar_y = body_y + vid_h + 36;
        int btn_y = bar_y + 16;
        int btn_size = 32;
        int btn_gap = 12;
        int total_btns = 3;
        int total_w = btn_size * total_btns + btn_gap * (total_btns - 1);
        int btn_x = body_x + (w->w - 32 - total_w) / 2;
        int play_x = btn_x + btn_size + btn_gap;

        if (e->mouse.x >= play_x && e->mouse.x < play_x + btn_size &&
            e->mouse.y >= btn_y && e->mouse.y < btn_y + btn_size) {
            st->playing = !st->playing;
            if (st->playing) {
                strcpy(st->track_name, "demo.mp4 (no codec)");
            } else {
                st->track_name[0] = '\0';
                st->progress = 0;
            }
        }
    }
}

struct widget* media_create(int x, int y) {
    media_state.active = 0;
    media_state.playing = 0;
    media_state.progress = 0;
    media_state.track_name[0] = '\0';

    media_widget.x = x;
    media_widget.y = y;
    media_widget.w = MEDIA_W;
    media_widget.h = MEDIA_H;
    media_widget.visible = 1;
    media_widget.focused = 0;
    media_widget.draggable = 1;
    media_widget.resizable = 0;
    media_widget.draw = media_draw;
    media_widget.on_event = media_on_event;
    media_widget.state = &media_state;
    memcpy(media_widget.title, "Media", 6);
    return &media_widget;
}
