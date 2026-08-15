/*
 * Lestra OS - Theme system
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * Centralised design-token table with two variants (dark + light) and
 * a per-user accent color. The chosen theme is persisted to /etc/theme
 * as a single line: "<variant> <accent_index>".
 *
 * Other GUI files should call theme_text_primary(), theme_accent(),
 * etc. instead of hard-coding UI_* constants when they want to respect
 * the user's theme choice.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/gui.h>
#include <lestra/vfs.h>
#include <lestra/printk.h>
#include <string.h>

#define THEME_PATH "/etc/theme"

enum {
    THEME_DARK = 0,
    THEME_LIGHT = 1,
};

/* Two token tables, indexed by variant. Each row is a named slot. */
enum {
    TK_BG_BASE = 0,
    TK_BG_GRAD_TOP,
    TK_BG_GRAD_BOT,
    TK_ACCENT,
    TK_ACCENT_HOT,
    TK_ACCENT_SOFT,
    TK_TEXT_PRIMARY,
    TK_TEXT_MUTED,
    TK_TEXT_FAINT,
    TK_CARD_BG,
    TK_CARD_BORDER,
    TK_DANGER,
    TK_SUCCESS,
    TK_COUNT
};

static uint32_t theme_tokens[2][TK_COUNT] = {
    /* Dark variant — matches the existing UI_* palette in fb.h. */
    [THEME_DARK] = {
        [TK_BG_BASE]      = 0xFF0A0C12,
        [TK_BG_GRAD_TOP]  = 0xFF0E1422,
        [TK_BG_GRAD_BOT]  = 0xFF050608,
        [TK_ACCENT]       = 0xFF22D3EE,
        [TK_ACCENT_HOT]   = 0xFF06B6D4,
        [TK_ACCENT_SOFT]  = 0xFF67E8F9,
        [TK_TEXT_PRIMARY] = 0xFFE7F0F5,
        [TK_TEXT_MUTED]   = 0xFF94A3B8,
        [TK_TEXT_FAINT]   = 0xFF475569,
        [TK_CARD_BG]      = 0xC7121620,
        [TK_CARD_BORDER]  = 0x2E22D3EE,
        [TK_DANGER]       = 0xFFF87171,
        [TK_SUCCESS]      = 0xFF4ADE80,
    },
    /* Light variant — bright slate background, dark text. */
    [THEME_LIGHT] = {
        [TK_BG_BASE]      = 0xFFF1F5F9,
        [TK_BG_GRAD_TOP]  = 0xFFE2E8F0,
        [TK_BG_GRAD_BOT]  = 0xFFF8FAFC,
        [TK_ACCENT]       = 0xFF0891B2,
        [TK_ACCENT_HOT]   = 0xFF06B6D4,
        [TK_ACCENT_SOFT]  = 0xFF67E8F9,
        [TK_TEXT_PRIMARY] = 0xFF0F172A,
        [TK_TEXT_MUTED]   = 0xFF475569,
        [TK_TEXT_FAINT]   = 0xFF94A3B8,
        [TK_CARD_BG]      = 0xCCFFFFFF,
        [TK_CARD_BORDER]  = 0x2E0891B2,
        [TK_DANGER]       = 0xFFDC2626,
        [TK_SUCCESS]      = 0xFF16A34A,
    }
};

/* Accent palette — 6 options, indexed 0..5. */
static uint32_t theme_accents[6] = {
    0xFF22D3EE, 0xFFF87171, 0xFF4ADE80,
    0xFFFBBF24, 0xFFA78BFA, 0xFFEC4899,
};

struct theme_state {
    int variant;
    int accent_idx;
    int inited;
};

static struct theme_state theme_state;

/* ---------- helpers ---------- */
static void theme_load(void) {
    theme_state.variant = THEME_DARK;
    theme_state.accent_idx = 0;
    theme_state.inited = 1;

    int fd = vfs_open(THEME_PATH, O_RDONLY);
    if (fd < 0) return;
    char buf[32];
    ssize_t n = vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    /* Parse "<variant> <accent>" */
    int v = 0, a = 0;
    /* find first digit */
    const char* p = buf;
    while (*p && (*p < '0' || *p > '9')) p++;
    if (*p >= '0' && *p <= '9') {
        v = *p - '0'; p++;
        while (*p && (*p < '0' || *p > '9')) p++;
        if (*p >= '0' && *p <= '9') a = *p - '0';
    }
    if (v == THEME_DARK || v == THEME_LIGHT) theme_state.variant = v;
    if (a >= 0 && a < 6) theme_state.accent_idx = a;
}

static void theme_save(void) {
    int fd = vfs_open(THEME_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    char buf[16];
    int n = ksnprintf(buf, sizeof(buf), "%d %d\n",
                      theme_state.variant, theme_state.accent_idx);
    vfs_write(fd, buf, n);
    vfs_close(fd);
}

/* ---------- public API ---------- */
void theme_init(void) {
    if (theme_state.inited) return;
    theme_load();
    pr_info("theme: initialised (variant=%s, accent=%d)\n",
            theme_state.variant == THEME_DARK ? "dark" : "light",
            theme_state.accent_idx);
}

void theme_set(int variant, int accent_idx) {
    if (!theme_state.inited) theme_init();
    if (variant == THEME_DARK || variant == THEME_LIGHT) {
        theme_state.variant = variant;
    }
    if (accent_idx >= 0 && accent_idx < 6) {
        theme_state.accent_idx = accent_idx;
    }
    /* Override the accent token in the active table. */
    theme_tokens[theme_state.variant][TK_ACCENT]      = theme_accents[theme_state.accent_idx];
    theme_tokens[theme_state.variant][TK_ACCENT_HOT]  = theme_accents[theme_state.accent_idx];
    theme_save();
}

void theme_toggle(void) {
    if (!theme_state.inited) theme_init();
    theme_state.variant = (theme_state.variant == THEME_DARK)
                          ? THEME_LIGHT : THEME_DARK;
    theme_tokens[theme_state.variant][TK_ACCENT]      = theme_accents[theme_state.accent_idx];
    theme_tokens[theme_state.variant][TK_ACCENT_HOT]  = theme_accents[theme_state.accent_idx];
    theme_save();
}

uint32_t theme_accent(void) {
    if (!theme_state.inited) theme_init();
    return theme_tokens[theme_state.variant][TK_ACCENT];
}

uint32_t theme_text_primary(void) {
    if (!theme_state.inited) theme_init();
    return theme_tokens[theme_state.variant][TK_TEXT_PRIMARY];
}

uint32_t theme_text_muted(void) {
    if (!theme_state.inited) theme_init();
    return theme_tokens[theme_state.variant][TK_TEXT_MUTED];
}

uint32_t theme_bg_base(void) {
    if (!theme_state.inited) theme_init();
    return theme_tokens[theme_state.variant][TK_BG_BASE];
}

uint32_t theme_card_bg(void) {
    if (!theme_state.inited) theme_init();
    return theme_tokens[theme_state.variant][TK_CARD_BG];
}

uint32_t theme_card_border(void) {
    if (!theme_state.inited) theme_init();
    return theme_tokens[theme_state.variant][TK_CARD_BORDER];
}

uint32_t theme_danger(void) {
    if (!theme_state.inited) theme_init();
    return theme_tokens[theme_state.variant][TK_DANGER];
}

uint32_t theme_success(void) {
    if (!theme_state.inited) theme_init();
    return theme_tokens[theme_state.variant][TK_SUCCESS];
}

int theme_variant(void) {
    if (!theme_state.inited) theme_init();
    return theme_state.variant;
}

int theme_accent_index(void) {
    if (!theme_state.inited) theme_init();
    return theme_state.accent_idx;
}
