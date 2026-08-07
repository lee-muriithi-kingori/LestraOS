/*
 * Lestra OS - Dynamic Island public API
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * The Dynamic Island is a morphing pill at the top-center of the screen
 * that displays the current foreground activity: idle clock, music
 * playback, downloads, AI generation, voice input, or transient
 * notifications. Activities are queued by priority and the island
 * rotates between them every few seconds.
 *
 * Higher priority activities pre-empt the current one immediately.
 * Duration-bounded activities (notifications) auto-expire; persistent
 * activities (music, voice, AI) stay until dismissed.
 *
 * See kernel/gui/dynamic_island.c for the renderer.
 */
#ifndef LESTRA_DYNAMIC_ISLAND_H
#define LESTRA_DYNAMIC_ISLAND_H

#include <lestra/types.h>
#include <lestra/input.h>

/* Activity states the island can morph between. */
typedef enum {
    ISLAND_IDLE = 0,
    ISLAND_MUSIC,
    ISLAND_DOWNLOAD,
    ISLAND_AI,
    ISLAND_BACKGROUND,
    ISLAND_VOICE,
    ISLAND_NOTIFICATION,
    ISLAND_TIMER,
} island_state_t;

/* One queued activity. Multiple can be pending; the island shows the
 * highest-priority one and rotates every 3 s. */
struct island_activity {
    island_state_t state;
    uint64_t       start_ms;       /* filled in by island_push() */
    uint64_t       duration_ms;    /* 0 = persistent until dismissed */
    int            priority;       /* higher wins */
    char           text1[64];
    char           text2[64];
    uint32_t       color;
    int            progress;       /* 0..100 (used by music/download) */
};

/* Lifecycle / generic */
void island_init(void);
void island_push(struct island_activity* a);
void island_dismiss(void);
void island_render(void);
int  island_handle_event(struct event* e);

/* ---- Convenience pushers ----
 * Each one fills in an island_activity and calls island_push(). */
void island_notify_music(const char* track, const char* artist, uint32_t color);
void island_notify_download_start(const char* app, int progress);
void island_update_download(const char* app, int progress);
void island_notify_ai_running(const char* model);
void island_notify_voice_start(void);
void island_notify_notification(const char* title, const char* body, uint32_t color);

/* ---- Dismiss helpers ----
 * Each dismiss routine clears the matching slot. They all call the
 * generic island_dismiss() under the hood since the island only shows
 * one activity at a time. */
void island_dismiss_music(void);
void island_dismiss_download(void);
void island_dismiss_ai(void);
void island_notify_voice_stop(void);
void island_dismiss_notification(void);

#endif /* LESTRA_DYNAMIC_ISLAND_H */
