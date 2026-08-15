/*
 * Lestra OS - Animation engine
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * A tiny fixed-point animation engine for UI transitions. The kernel
 * has no FPU/SSE (we compile with -mno-sse), so all math is integer.
 *
 * API:
 *   anim_start(id, from, to, duration_ms, easing)
 *       Begin animating slot `id` from `from` to `to` over the given
 *       duration. `id` must be in [0, ANIM_SLOTS).
 *   anim_update(now_ms)
 *       Advance every active animation to the current time. Returns
 *       the number of animations that completed this tick.
 *   anim_get(id)
 *       Returns the current integer value of animation `id`, or the
 *       `to` value if the animation has finished.
 *   anim_stop(id)
 *       Cancel animation `id`. Its value freezes at the last sampled
 *       position.
 *   anim_is_active(id)
 *       1 if the animation is still running, 0 otherwise.
 *
 * Easing functions: EASE_OUT_CUBIC, EASE_IN_OUT, LINEAR, BOUNCE.
 *
 * All math uses millisecond timestamps from timer_get_ms() and a
 * 1000-scale fixed-point for the easing fractions.
 */
#ifndef LESTRA_ANIMATIONS_H
#define LESTRA_ANIMATIONS_H

#include <lestra/types.h>

#define ANIM_SLOTS  24

typedef enum {
    EASE_LINEAR = 0,
    EASE_OUT_CUBIC,
    EASE_IN_OUT,
    BOUNCE,
} anim_easing_t;

void anim_start(int id, int from, int to, uint32_t duration_ms, anim_easing_t e);
void anim_stop(int id);
int  anim_is_active(int id);
int  anim_get(int id);
int  anim_update(uint64_t now_ms);

/* Convenience: advance every active animation using the current timer
 * value. Returns the number of animations that completed this tick. */
int  anim_tick(void);

/* Convenience: same as anim_get but returns the eased progress as a
 * 0..1000 fraction instead of the interpolated value. Useful when you
 * want to drive multiple visual properties from a single animation. */
int  anim_get_progress_1000(int id);

#endif /* LESTRA_ANIMATIONS_H */
