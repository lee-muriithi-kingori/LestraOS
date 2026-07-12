/*
 * Lestra OS - Dynamic Island — living top bar that morphs based on activity
 */
#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/font.h>
#include <lestra/timer.h>
#include <lestra/printk.h>
#include <string.h>

#define COL_ISLAND_BG    0xD90D0F14u
#define COL_ISLAND_BORDER 0x6600D4FFu
#define COL_ISLAND_GLOW   0x4000D4FFu
#define COL_TEXT          0xFFF0F0F0u
#define COL_TEXT_DIM      0xFF8B8B8Bu
#define COL_ACCENT        0xFF00D4FFu
#define COL_DANGER        0xFFFF4444u

typedef enum {
    ISLAND_IDLE, ISLAND_MUSIC, ISLAND_DOWNLOAD, ISLAND_AI,
    ISLAND_BACKGROUND, ISLAND_VOICE, ISLAND_NOTIFICATION, ISLAND_TIMER
} island_state_t;

struct island_activity {
    island_state_t state;
    uint64_t start_ms;
    uint64_t duration_ms;
    int priority;
    char text1[64];
    char text2[64];
    uint32_t color;
    int progress;
};

#define MAX_ISLAND_SLOTS 8
static struct island_activity island_queue[MAX_ISLAND_SLOTS];
static int island_current_idx = -1;
static uint64_t island_last_rotate_ms = 0;

static int island_target_w = 200;
static int island_current_w = 200;
static int island_target_h = 36;
static int island_current_h = 36;

void island_init(void) {
    memset(island_queue, 0, sizeof(island_queue));
    island_current_idx = -1;
    pr_info("dynamic_island: initialized (8 states, 8-slot queue)\n");
}

void island_push(struct island_activity* a) {
    if (!a) return;
    for (int i = 0; i < MAX_ISLAND_SLOTS; i++) {
        if (!island_queue[i].state) {
            island_queue[i] = *a;
            island_queue[i].start_ms = timer_get_ms();
            if (island_current_idx < 0 || a->priority > island_queue[island_current_idx].priority) {
                island_current_idx = i;
                island_last_rotate_ms = timer_get_ms();
            }
            return;
        }
    }
}

void island_dismiss(void) {
    if (island_current_idx >= 0) {
        island_queue[island_current_idx].state = 0;
        island_current_idx = -1;
        for (int i = 0; i < MAX_ISLAND_SLOTS; i++) {
            if (island_queue[i].state) { island_current_idx = i; break; }
        }
    }
}

static int island_pick_next(void) {
    int best = -1;
    int best_pri = -1;
    uint64_t now = timer_get_ms();
    for (int i = 0; i < MAX_ISLAND_SLOTS; i++) {
        if (!island_queue[i].state) continue;
        if (island_queue[i].duration_ms > 0 && (now - island_queue[i].start_ms) > island_queue[i].duration_ms) {
            island_queue[i].state = 0;
            continue;
        }
        if (island_queue[i].priority > best_pri) {
            best_pri = island_queue[i].priority;
            best = i;
        }
    }
    return best;
}

static void island_animate(int target_w, int target_h) {
    island_target_w = target_w;
    island_target_h = target_h;
    int diff_w = island_target_w - island_current_w;
    int diff_h = island_target_h - island_current_h;
    if (diff_w > 0) island_current_w += (diff_w + 3) / 4;
    else if (diff_w < 0) island_current_w += (diff_w - 3) / 4;
    if (diff_h > 0) island_current_h += (diff_h + 3) / 4;
    else if (diff_h < 0) island_current_h += (diff_h - 3) / 4;
    if (diff_w >= 0 && diff_w < 4) island_current_w = island_target_w;
    if (diff_h >= 0 && diff_h < 4) island_current_h = island_target_h;
}

void island_render(void) {
    uint64_t now = timer_get_ms();

    /* Rotate every 3s if multiple activities */
    if (now - island_last_rotate_ms > 3000) {
        int next = island_pick_next();
        if (next >= 0 && next != island_current_idx) {
            island_current_idx = next;
            island_last_rotate_ms = now;
        }
    }

    /* Auto-expire */
    if (island_current_idx >= 0) {
        struct island_activity* a = &island_queue[island_current_idx];
        if (a->duration_ms > 0 && (now - a->start_ms) > a->duration_ms) {
            a->state = 0;
            island_current_idx = island_pick_next();
        }
    }

    if (island_current_idx < 0) {
        island_current_idx = island_pick_next();
        island_last_rotate_ms = now;
    }

    /* Determine target size based on state */
    int tw = 200, th = 36;
    if (island_current_idx >= 0) {
        struct island_activity* a = &island_queue[island_current_idx];
        switch (a->state) {
            case ISLAND_IDLE: tw = 200; th = 36; break;
            case ISLAND_MUSIC: tw = 400; th = 44; break;
            case ISLAND_DOWNLOAD: tw = 350; th = 44; break;
            case ISLAND_AI: tw = 400; th = 44; break;
            case ISLAND_VOICE: tw = 400; th = 52; break;
            case ISLAND_NOTIFICATION: tw = 350; th = 44; break;
            case ISLAND_BACKGROUND: tw = 300; th = 40; break;
            case ISLAND_TIMER: tw = 250; th = 40; break;
            default: tw = 200; th = 36; break;
        }
    }
    island_animate(tw, th);

    int iw = island_current_w;
    int ih = island_current_h;
    int ix = (int)fb_w / 2 - iw / 2;
    int iy = 12;

    /* Breathing glow border */
    int breath = 0;
    {
        extern uint64_t timer_get_ms(void);
        uint64_t t = timer_get_ms();
        int s = (int)((t * 360) / 2400);
        /* sin approx */
        static const int8_t sin_t[64] = {
            0,6,12,18,24,29,35,40,45,49,53,56,59,61,62,63,63,62,61,59,56,53,49,45,40,35,29,24,18,12,6,0,
            -6,-12,-18,-24,-29,-35,-40,-45,-49,-53,-56,-59,-61,-62,-63,-63,-62,-61,-59,-56,-53,-49,-45,-40,-35,-29,-24,-18,-12,-6,0
        };
        breath = sin_t[(s * 64 / 360) & 63];
    }
    uint32_t glow_alpha = 0x30 + ((breath + 63) * 0x10) / 63;
    if (glow_alpha > 0x60) glow_alpha = 0x60;

    /* Draw glow */
    fb_draw_rounded(ix - 4, iy - 4, iw + 8, ih + 8, (ih + 8) / 2,
                    (glow_alpha << 24) | 0x0000D4FFu, (glow_alpha << 24) | 0x0000D4FFu);

    /* Draw pill body */
    fb_draw_rounded(ix, iy, iw, ih, ih / 2, COL_ISLAND_BG, COL_ISLAND_BG);
    fb_draw_rounded(ix, iy, iw, ih, ih / 2, 0, COL_ISLAND_BORDER);

    /* Render content */
    char buf[128];
    if (island_current_idx >= 0) {
        struct island_activity* a = &island_queue[island_current_idx];
        switch (a->state) {
            case ISLAND_MUSIC:
                fb_fill_rect(ix + 12, iy + 8, 28, 28, a->color);
                fb_draw_string(ix + 48, iy + 6, a->text1, COL_TEXT);
                fb_draw_string(ix + 48, iy + 22, a->text2, COL_TEXT_DIM);
                /* Progress bar */
                fb_draw_rounded(ix + 48, iy + ih - 8, iw - 80, 3, 2, 0xFF333333u, 0xFF333333u);
                fb_draw_rounded(ix + 48, iy + ih - 8, (iw - 80) * a->progress / 100, 3, 2, COL_ACCENT, COL_ACCENT);
                break;
            case ISLAND_DOWNLOAD:
                fb_draw_string(ix + 16, iy + 6, a->text1, COL_TEXT);
                ksnprintf(buf, sizeof(buf), "%d%%", a->progress);
                fb_draw_string(ix + 16, iy + 22, buf, COL_TEXT_DIM);
                fb_draw_rounded(ix + 16, iy + ih - 8, iw - 32, 3, 2, 0xFF333333u, 0xFF333333u);
                fb_draw_rounded(ix + 16, iy + ih - 8, (iw - 32) * a->progress / 100, 3, 2, COL_ACCENT, COL_ACCENT);
                break;
            case ISLAND_AI:
                fb_fill_rect(ix + 12, iy + 10, 24, 24, COL_ACCENT);
                fb_draw_string(ix + 44, iy + 6, a->text1, COL_TEXT);
                fb_draw_string(ix + 44, iy + 22, a->text2, COL_TEXT_DIM);
                break;
            case ISLAND_VOICE:
                fb_fill_rect(ix + 12, iy + 10, 16, 24, COL_DANGER);
                fb_draw_string(ix + 36, iy + 4, a->text1, COL_TEXT);
                /* Waveform */
                for (int i = 0; i < 16; i++) {
                    int amp = 4 + ((i * 7 + (int)(now / 50)) % 12);
                    int bh = amp * (ih - 16) / 15;
                    fb_fill_rect(ix + 36 + i * 6, iy + (ih - bh) / 2, 4, bh, COL_DANGER);
                }
                break;
            case ISLAND_NOTIFICATION:
                fb_fill_rect(ix + 12, iy + 8, 28, 28, a->color);
                fb_draw_string(ix + 48, iy + 6, a->text1, COL_TEXT);
                fb_draw_string(ix + 48, iy + 22, a->text2, COL_TEXT_DIM);
                break;
            default:
                /* IDLE: clock + battery */
                {
                    extern void rtc_get_time(uint8_t*, uint8_t*, uint8_t*);
                    uint8_t h, m, s;
                    rtc_get_time(&h, &m, &s);
                    ksnprintf(buf, sizeof(buf), "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
                    fb_draw_string(ix + 20, iy + (ih - 16) / 2, buf, COL_TEXT);
                }
                break;
        }
    } else {
        /* IDLE state */
        extern void rtc_get_time(uint8_t*, uint8_t*, uint8_t*);
        uint8_t h, m, s;
        rtc_get_time(&h, &m, &s);
        ksnprintf(buf, sizeof(buf), "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
        fb_draw_string(ix + 20, iy + (ih - 16) / 2, buf, COL_TEXT);
    }
}

int island_handle_event(struct event* e) {
    (void)e;
    return 0;
}

/* Convenience pushers */
void island_notify_music(const char* track, const char* artist, uint32_t color) {
    struct island_activity a = {0};
    a.state = ISLAND_MUSIC; a.priority = 5; a.color = color; a.progress = 0;
    strncpy(a.text1, track ? track : "", 63);
    strncpy(a.text2, artist ? artist : "", 63);
    island_push(&a);
}
void island_dismiss_music(void) { island_dismiss(); }

void island_notify_download_start(const char* app, int progress) {
    struct island_activity a = {0};
    a.state = ISLAND_DOWNLOAD; a.priority = 8; a.progress = progress;
    strncpy(a.text1, app ? app : "", 63);
    island_push(&a);
}
void island_update_download(const char* app, int progress) {
    for (int i = 0; i < MAX_ISLAND_SLOTS; i++) {
        if (island_queue[i].state == ISLAND_DOWNLOAD && strcmp(island_queue[i].text1, app) == 0) {
            island_queue[i].progress = progress; return;
        }
    }
}
void island_dismiss_download(void) { island_dismiss(); }

void island_notify_ai_running(const char* model) {
    struct island_activity a = {0};
    a.state = ISLAND_AI; a.priority = 7;
    strncpy(a.text1, model ? model : "AI", 63);
    strcpy(a.text2, "generating...");
    island_push(&a);
}
void island_dismiss_ai(void) { island_dismiss(); }

void island_notify_voice_start(void) {
    struct island_activity a = {0};
    a.state = ISLAND_VOICE; a.priority = 9;
    strcpy(a.text1, "Listening...");
    island_push(&a);
}
void island_notify_voice_stop(void) { island_dismiss(); }

void island_notify_notification(const char* title, const char* body, uint32_t color) {
    struct island_activity a = {0};
    a.state = ISLAND_NOTIFICATION; a.priority = 6; a.duration_ms = 3000; a.color = color;
    strncpy(a.text1, title ? title : "", 63);
    strncpy(a.text2, body ? body : "", 63);
    island_push(&a);
}
