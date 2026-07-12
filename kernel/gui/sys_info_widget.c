/*
 * Lestra OS - System Info Widget
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * A read-only panel that summarises the running system:
 *   - OS name + version
 *   - Kernel build target (x86_64, multiboot2)
 *   - CPU vendor + core count (via CPUID)
 *   - RAM total / used / free
 *   - IP address
 *   - Disk size (we approximate using pmm_get_total())
 *
 * Refreshes every 5 seconds (the timestamp ticks down in the corner).
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <lestra/mm.h>
#include <lestra/net.h>
#include <lestra/printk.h>
#include <string.h>

#define SI_W    360
#define SI_H    320
#define SI_TITLE_H 36
#define SI_PAD  8

struct si_state {
    char cpu_vendor[32];
    int  cpu_cores;
    int  cpu_freq_mhz;
    uint64_t last_refresh_ms;
    int  refresh_in_secs;
};

static struct si_state si_state;
static struct widget   si_widget;

/* ---------- CPUID ---------- */
static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "0"(leaf), "2"(subleaf));
}

static void si_probe_cpu(struct si_state* st) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    char* v = st->cpu_vendor;
    v[0] = (char)((b >> 0)  & 0xFF); v[1] = (char)((b >> 8)  & 0xFF);
    v[2] = (char)((b >> 16) & 0xFF); v[3] = (char)((b >> 24) & 0xFF);
    v[4] = (char)((d >> 0)  & 0xFF); v[5] = (char)((d >> 8)  & 0xFF);
    v[6] = (char)((d >> 16) & 0xFF); v[7] = (char)((d >> 24) & 0xFF);
    v[8] = (char)((c >> 0)  & 0xFF); v[9] = (char)((c >> 8)  & 0xFF);
    v[10] = (char)((c >> 16) & 0xFF); v[11] = (char)((c >> 24) & 0xFF);
    v[12] = '\0';

    cpuid(1, 0, &a, &b, &c, &d);
    st->cpu_cores = (int)((b >> 16) & 0xFF);
    if (st->cpu_cores < 1) st->cpu_cores = 1;
    /* Frequency: quick 5 ms calibration. */
    uint64_t t0 = rdtsc();
    uint64_t start_ms = timer_get_ms();
    while (timer_get_ms() - start_ms < 5) { /* spin */ }
    uint64_t t1 = rdtsc();
    st->cpu_freq_mhz = (int)((t1 - t0) / 5000);
    if (st->cpu_freq_mhz < 10) st->cpu_freq_mhz = 2000;
}

/* ---------- draw ---------- */
static void si_draw_row(int x, int y, int w, const char* label,
                        const char* value, uint32_t color) {
    fb_draw_string(x, y, label, UI_TEXT_MUTED);
    int lw = fb_text_width(label);
    fb_draw_string(x + lw + 12, y, value, color);
    (void)w;
}

static void si_draw(struct widget* w) {
    extern void ui_draw_card(int x, int y, int w, int h, int focused);
    ui_draw_card(w->x, w->y, w->w, w->h, w->focused);
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, SI_TITLE_H - 1, 0xE00E1422);
    fb_draw_string(w->x + 12, w->y + 10, "System Info", UI_TEXT_PRIMARY);
    fb_draw_string(w->x + w->w - 20, w->y + 10, "x", UI_TEXT_MUTED);

    uint64_t now = timer_get_ms();
    if (si_state.cpu_cores == 0) {
        si_probe_cpu(&si_state);
        si_state.last_refresh_ms = now;
    }
    /* Refresh every 5 s. */
    if (now - si_state.last_refresh_ms > 5000) {
        si_state.last_refresh_ms = now;
    }
    si_state.refresh_in_secs =
        5 - (int)((now - si_state.last_refresh_ms) / 1000);
    if (si_state.refresh_in_secs < 0) si_state.refresh_in_secs = 0;

    int bx = w->x + SI_PAD + 4;
    int by = w->y + SI_TITLE_H + SI_PAD;
    int bw = w->w - 2 * SI_PAD;

    /* OS / kernel. */
    si_draw_row(bx, by, bw, "OS:", "Lestra OS 1.0.0-alpha",
                UI_ACCENT_SOFT); by += 20;
    si_draw_row(bx, by, bw, "Kernel:", "lestra-kernel (x86_64)",
                UI_TEXT_PRIMARY); by += 20;
    si_draw_row(bx, by, bw, "Bootloader:", "GRUB / multiboot2",
                UI_TEXT_PRIMARY); by += 20;
    si_draw_row(bx, by, bw, "Framebuffer:", "VESA 1024x768x32",
                UI_TEXT_PRIMARY); by += 24;

    /* CPU. */
    si_draw_row(bx, by, bw, "CPU:", si_state.cpu_vendor,
                UI_TEXT_PRIMARY); by += 20;
    char cbuf[32];
    ksnprintf(cbuf, sizeof(cbuf), "%d cores @ %d MHz",
              si_state.cpu_cores, si_state.cpu_freq_mhz);
    si_draw_row(bx, by, bw, "Cores:", cbuf, UI_TEXT_PRIMARY); by += 24;

    /* RAM. */
    uintptr_t total = pmm_get_total();
    uintptr_t used  = pmm_get_used();
    uintptr_t free  = pmm_get_free();
    char rbuf[64];
    ksnprintf(rbuf, sizeof(rbuf), "%u MB / %u MB",
              (unsigned)(used  / (1024 * 1024)),
              (unsigned)(total / (1024 * 1024)));
    si_draw_row(bx, by, bw, "Memory:", rbuf, UI_TEXT_PRIMARY); by += 20;
    ksnprintf(rbuf, sizeof(rbuf), "%u MB free",
              (unsigned)(free / (1024 * 1024)));
    si_draw_row(bx, by, bw, "  Free:", rbuf, UI_SUCCESS); by += 24;

    /* Network. */
    ipv4_addr_t ip = net_get_ip();
    char nbuf[32];
    ksnprintf(nbuf, sizeof(nbuf), "%u.%u.%u.%u",
              ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3]);
    si_draw_row(bx, by, bw, "IP:", nbuf,
                net_is_up() ? UI_TEXT_PRIMARY : UI_DANGER); by += 20;
    si_draw_row(bx, by, bw, "Net:",
                net_is_up() ? "UP" : "DOWN",
                net_is_up() ? UI_SUCCESS : UI_DANGER); by += 24;

    /* Disk: approximate. */
    char dbuf[32];
    ksnprintf(dbuf, sizeof(dbuf), "%u MB",
              (unsigned)(total / (1024 * 1024)));
    si_draw_row(bx, by, bw, "Disk (est.):", dbuf, UI_TEXT_PRIMARY);
    by += 24;

    /* Refresh countdown. */
    char rc[32];
    ksnprintf(rc, sizeof(rc), "Refresh in %ds", si_state.refresh_in_secs);
    fb_draw_string_small(w->x + SI_PAD, w->y + w->h - 16,
                         rc, UI_TEXT_MUTED);
}

static void si_on_event(struct widget* w, struct event* e) {
    if (e->type != EV_MOUSE_DOWN) return;
    int mx = e->mouse.x, my = e->mouse.y;
    int cx = w->x + w->w - 24, cy = w->y + 10;
    if (mx >= cx && mx < cx + 16 && my >= cy && my < cy + 16) {
        w->visible = 0;
        return;
    }
    /* Click body = force refresh. */
    si_state.last_refresh_ms = timer_get_ms() - 5001;
}

/* ---------- public ---------- */
struct widget* sys_info_widget_create(int x, int y) {
    memset(&si_state, 0, sizeof(si_state));
    si_probe_cpu(&si_state);
    si_state.last_refresh_ms = timer_get_ms();

    si_widget.x = x;
    si_widget.y = y;
    si_widget.w = SI_W;
    si_widget.h = SI_H;
    si_widget.visible = 1;
    si_widget.focused = 1;
    si_widget.draggable = 1;
    si_widget.resizable = 0;
    si_widget.draw = si_draw;
    si_widget.on_event = si_on_event;
    si_widget.state = &si_state;
    memcpy(si_widget.title, "System Info", 12);
    return &si_widget;
}
