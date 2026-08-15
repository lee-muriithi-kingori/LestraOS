/*
 * Lestra OS - Animation engine
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Integer-only fixed-point animation engine. 24 slots, no FPU/SSE.
 * See include/lestra/animations.h for the API contract.
 */

#include <lestra/types.h>
#include <lestra/animations.h>
#include <lestra/timer.h>
#include <lestra/printk.h>
#include <string.h>

struct anim {
    int       active;
    int       from;
    int       to;
    uint64_t  start_ms;
    uint32_t  duration_ms;
    anim_easing_t easing;
    int       last_value;
};

static struct anim anims[ANIM_SLOTS];

void anim_start(int id, int from, int to, uint32_t duration_ms, anim_easing_t e) {
    if (id < 0 || id >= ANIM_SLOTS) return;
    anims[id].active      = 1;
    anims[id].from        = from;
    anims[id].to          = to;
    anims[id].start_ms    = timer_get_ms();
    anims[id].duration_ms = duration_ms ? duration_ms : 1;
    anims[id].easing      = e;
    anims[id].last_value  = from;
}

void anim_stop(int id) {
    if (id < 0 || id >= ANIM_SLOTS) return;
    anims[id].active = 0;
}

int anim_is_active(int id) {
    if (id < 0 || id >= ANIM_SLOTS) return 0;
    return anims[id].active;
}

/* All easing helpers operate on a 0..1000 progress fraction and
 * return a 0..1000 eased fraction. */

static int ease_linear(int t01) {
    if (t01 < 0) t01 = 0;
    if (t01 > 1000) t01 = 1000;
    return t01;
}

static int ease_out_cubic_fn(int t01) {
    if (t01 < 0) t01 = 0;
    if (t01 > 1000) t01 = 1000;
    int inv = 1000 - t01;
    /* inv^3 / 1000^2 (so result is in 0..1000). */
    int inv3 = (inv * inv / 1000) * inv / 1000;
    return 1000 - inv3;
}

static int ease_in_out_fn(int t01) {
    if (t01 < 0) t01 = 0;
    if (t01 > 1000) t01 = 1000;
    /* Piecewise: linear for first half, ease-out for second half. */
    if (t01 < 500) return t01;
    int inv = 1000 - t01;
    int inv3 = (inv * inv / 1000) * inv / 1000;
    return 1000 - inv3;
}

static int ease_bounce_fn(int t01) {
    if (t01 < 0) t01 = 0;
    if (t01 > 1000) t01 = 1000;
    /* Bounce-out: piecewise quadratic bumps. */
    if (t01 < 364) {
        int x = t01 * 1000 / 364;
        return (x * x) / 1000 * 1000 / 1000;   /* 0..1 */
    } else if (t01 < 728) {
        int x = (t01 - 546) * 1000 / 364;
        return 750 + (x * x) / 4000;
    } else if (t01 < 909) {
        int x = (t01 - 819) * 1000 / 364;
        return 937 + (x * x) / 16000;
    } else {
        int x = (t01 - 954) * 1000 / 364;
        return 984 + (x * x) / 64000;
    }
}

static int apply_easing(anim_easing_t e, int t01) {
    switch (e) {
        case EASE_OUT_CUBIC: return ease_out_cubic_fn(t01);
        case EASE_IN_OUT:    return ease_in_out_fn(t01);
        case BOUNCE:         return ease_bounce_fn(t01);
        case EASE_LINEAR:
        default:             return ease_linear(t01);
    }
}

int anim_get_progress_1000(int id) {
    if (id < 0 || id >= ANIM_SLOTS) return 1000;
    if (!anims[id].active) return 1000;
    uint64_t now = timer_get_ms();
    uint64_t elapsed = now - anims[id].start_ms;
    if (elapsed >= anims[id].duration_ms) return 1000;
    int t01 = (int)((elapsed * 1000) / anims[id].duration_ms);
    return apply_easing(anims[id].easing, t01);
}

int anim_get(int id) {
    if (id < 0 || id >= ANIM_SLOTS) return 0;
    if (!anims[id].active) return anims[id].last_value;
    uint64_t now = timer_get_ms();
    uint64_t elapsed = now - anims[id].start_ms;
    if (elapsed >= anims[id].duration_ms) {
        anims[id].last_value = anims[id].to;
        return anims[id].to;
    }
    int t01 = (int)((elapsed * 1000) / anims[id].duration_ms);
    int eased = apply_easing(anims[id].easing, t01);
    /* Linearly interpolate from->to using the eased fraction. */
    int v = anims[id].from +
            ((anims[id].to - anims[id].from) * eased) / 1000;
    anims[id].last_value = v;
    return v;
}

int anim_update(uint64_t now_ms) {
    int completed = 0;
    for (int i = 0; i < ANIM_SLOTS; i++) {
        if (!anims[i].active) continue;
        uint64_t elapsed = now_ms - anims[i].start_ms;
        if (elapsed >= anims[i].duration_ms) {
            anims[i].last_value = anims[i].to;
            anims[i].active = 0;
            completed++;
        } else {
            int t01 = (int)((elapsed * 1000) / anims[i].duration_ms);
            int eased = apply_easing(anims[i].easing, t01);
            anims[i].last_value = anims[i].from +
                ((anims[i].to - anims[i].from) * eased) / 1000;
        }
    }
    return completed;
}

int anim_tick(void) {
    return anim_update(timer_get_ms());
}
