/*
 * Lestra OS - GUI Compositor
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * A cooperative compositor that runs at ~60 Hz. It:
 *   - Pumps input events and dispatches them to widgets (z-ordered)
 *   - Renders the animated background (gradient + particles)
 *   - Renders all visible widgets (floating cards)
 *   - Renders the status pill and FAB
 *   - Swaps the double-buffered framebuffer
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/ui_pro.h>
#include <lestra/font.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <lestra/vga.h>
#include <lestra/net.h>
#include <lestra/mm.h>
#include <string.h>

/* ----- widget system ----- */
#define MAX_WIDGETS 32

/* Forward declarations for desktop icons (gui/desktop_icons.c) */
extern void desktop_icons_render(void);
extern int  desktop_icons_handle_click(int x, int y);

/* External: top floating bar (gui/top_bar.c) — animated, with STT */
extern void top_bar_init(void);
extern void top_bar_render(void);
extern int  top_bar_handle_click(int x, int y);

/* External: app icon grid (gui/app_grid.c) — Material icons + preinstalled */
extern void app_grid_init(void);
extern void app_grid_render(void);
extern int  app_grid_handle_click(int x, int y);

/* External: left drawer (gui/left_drawer.c) */
extern void left_drawer_render(void);
extern int  left_drawer_handle_event(struct event* e);
extern int  left_drawer_get_width(void);

/* External: dock (gui/dock.c) */
extern void dock_render(void);
extern int  dock_handle_event(struct event* e);

static struct widget* widgets[MAX_WIDGETS];
static int n_widgets = 0;
static int compositor_running = 0;

/* Mouse cursor state */
static int cursor_x = 512;
static int cursor_y = 384;
static int cursor_visible = 1;

/* Drag state */
static struct widget* drag_widget = NULL;
static int drag_off_x = 0;
static int drag_off_y = 0;

/* ----- xorshift32 PRNG for particles ----- */
static uint32_t xorshift32_state = 0x12345678u;
static uint32_t xorshift32(void) {
    uint32_t x = xorshift32_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xorshift32_state = x;
    return x;
}

/* ----- sin lookup table (kernel has no libm, no SSE) -----
 * 256-entry table for one full cycle. Returns -1000..1000. */
static const int16_t sin_table[256] = {
    0,25,50,74,98,125,150,175,200,224,249,273,297,321,345,369,
    392,415,438,460,482,504,525,546,566,586,605,624,642,660,676,692,
    707,721,734,746,757,766,775,782,788,793,797,799,800,800,799,796,
    792,787,780,772,762,751,739,726,711,695,678,660,640,620,598,575,
    551,526,500,473,445,417,388,358,327,296,264,232,200,167,134,101,
    67,34,0,-34,-67,-101,-134,-167,-200,-232,-264,-296,-327,-358,-388,-417,
    -445,-473,-500,-526,-551,-575,-598,-620,-640,-660,-678,-695,-711,-726,-739,-751,
    -762,-772,-780,-787,-792,-796,-799,-800,-800,-800,-797,-793,-788,-782,-775,-766,
    -757,-746,-734,-721,-707,-692,-676,-660,-642,-624,-605,-586,-566,-546,-525,-504,
    -482,-460,-438,-415,-392,-369,-345,-321,-297,-273,-249,-224,-200,-175,-150,-125,
    -98,-74,-50,-25,0,25,50,74,98,125,150,175,200,224,249,273,
    297,321,345,369,392,415,438,460,482,504,525,546,566,586,605,624,
    642,660,676,692,707,721,734,746,757,766,775,782,788,793,797,799,
    800,800,799,796,792,787,780,772,762,751,739,726,711,695,678,660,
    640,620,598,575,551,526,500,473,445,417,388,358,327,296,264,232,
    200,167,134,101,67,34,0
};

/* sin(phase) where phase is 0..1000 (0=0rad, 1000=2*pi). Returns -1000..1000. */
int isin(uint32_t phase_milli) {
    uint32_t idx = (phase_milli * 256) / 1000;
    return sin_table[idx & 0xFF];
}

/* ----- particles ----- */
#define NUM_PARTICLES 40
struct particle {
    int x, y;
    int vx, vy;
    int life;
    int max_life;
};
static struct particle particles[NUM_PARTICLES];

static void particles_init(void) {
    for (int i = 0; i < NUM_PARTICLES; i++) {
        particles[i].x = (int)(xorshift32() % fb_w);
        particles[i].y = (int)(xorshift32() % fb_h);
        particles[i].vx = (int)(xorshift32() % 3) - 1;
        particles[i].vy = (int)(xorshift32() % 3) - 1;
        particles[i].life = (int)(xorshift32() % 200) + 100;
        particles[i].max_life = particles[i].life;
    }
}

static void particles_update(void) {
    for (int i = 0; i < NUM_PARTICLES; i++) {
        particles[i].x += particles[i].vx;
        particles[i].y += particles[i].vy;
        particles[i].life--;
        if (particles[i].life <= 0 ||
            particles[i].x < 0 || particles[i].x >= (int)fb_w ||
            particles[i].y < 0 || particles[i].y >= (int)fb_h) {
            particles[i].x = (int)(xorshift32() % fb_w);
            particles[i].y = (int)(xorshift32() % fb_h);
            particles[i].vx = (int)(xorshift32() % 3) - 1;
            particles[i].vy = (int)(xorshift32() % 3) - 1;
            particles[i].life = (int)(xorshift32() % 200) + 100;
            particles[i].max_life = particles[i].life;
        }
    }
}

/* ----- background rendering ----- */
static void background_render(void) {
    /* Vertical gradient: top (0x0E1422) -> bottom (0x050608) */
    for (uint32_t y = 0; y < fb_h; y++) {
        uint32_t t = y * 256 / fb_h;
        uint8_t r = (uint8_t)((0x0E * (256 - t) + 0x05 * t) / 256);
        uint8_t g = (uint8_t)((0x14 * (256 - t) + 0x06 * t) / 256);
        uint8_t b = (uint8_t)((0x22 * (256 - t) + 0x08 * t) / 256);
        uint32_t color = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        uint32_t* row = &fb_back[y * fb_w];
        for (uint32_t x = 0; x < fb_w; x++) row[x] = color;
    }

    /* Particles */
    for (int i = 0; i < NUM_PARTICLES; i++) {
        int alpha = particles[i].life * 128 / particles[i].max_life;
        if (alpha < 0) alpha = 0;
        if (alpha > 128) alpha = 128;
        uint32_t color = ((uint32_t)alpha << 24) | 0x00475569u;
        fb_set_pixel(particles[i].x, particles[i].y,
                     fb_blend(fb_get_pixel(particles[i].x, particles[i].y), color));
    }

    /* Breathing radial glow under FAB */
    uint64_t now = timer_get_ms();
    /* 8-second cycle: phase = (now % 8000) * 1000 / 8000 = 0..999 */
    uint32_t phase = (uint32_t)((now % 8000) * 1000 / 8000);
    int glow_alpha = 10 + (4 * isin(phase)) / 1000;
    if (glow_alpha < 6) glow_alpha = 6;
    if (glow_alpha > 14) glow_alpha = 14;
    int gx = (int)fb_w - 128;
    int gy = (int)fb_h - 128;
    for (int dy = -80; dy <= 80; dy++) {
        for (int dx = -80; dx <= 80; dx++) {
            int dist_sq = dx * dx + dy * dy;
            if (dist_sq > 6400) continue;
            int falloff = 6400 - dist_sq;
            int a = (glow_alpha * falloff) / 6400;
            uint32_t src = ((uint32_t)a << 24) | 0x0067E8F9u;
            int px = gx + dx;
            int py = gy + dy;
            if (px >= 0 && px < (int)fb_w && py >= 0 && py < (int)fb_h) {
                fb_set_pixel(px, py, fb_blend(fb_get_pixel(px, py), src));
            }
        }
    }
}

/* ----- status pill ----- */
static void status_pill_render(void) {
    char buf[256];
    int len = 0;

    /* Real time from RTC */
    extern void rtc_get_time(uint8_t*, uint8_t*, uint8_t*);
    extern void rtc_get_date(uint16_t*, uint8_t*, uint8_t*);
    uint8_t hour, min, sec;
    uint16_t year;
    uint8_t month, day;
    rtc_get_time(&hour, &min, &sec);
    rtc_get_date(&year, &month, &day);
    len += ksnprintf(&buf[len], sizeof(buf) - len,
                     "%u:%02u:%02u", (unsigned)hour, (unsigned)min, (unsigned)sec);

    /* Battery */
    extern int battery_get_percent(void);
    extern int battery_is_charging(void);
    extern const char* battery_get_status_str(void);
    int bat_pct = battery_get_percent();
    if (bat_pct >= 0) {
        len += ksnprintf(&buf[len], sizeof(buf) - len,
                         "  BAT %u%%", (unsigned)bat_pct);
        if (battery_is_charging()) {
            len += ksnprintf(&buf[len], sizeof(buf) - len, "+");
        }
    }

    /* CPU temperature */
    extern int temp_get_cpu(void);
    int cpu_temp = temp_get_cpu();
    if (cpu_temp >= 0) {
        len += ksnprintf(&buf[len], sizeof(buf) - len,
                         "  CPU %uC", (unsigned)cpu_temp);
    }

    /* Memory */
    len += ksnprintf(&buf[len], sizeof(buf) - len,
                     "  MEM %u/%u MB",
                     (unsigned)(pmm_get_free() / (1024 * 1024)),
                     (unsigned)(pmm_get_total() / (1024 * 1024)));

    /* Network */
    if (net_is_up()) {
        extern int wifi_is_connected(void);
        if (wifi_is_connected()) {
            extern const char* wifi_get_connected_ssid(void);
            const char* ssid = wifi_get_connected_ssid();
            if (ssid) {
                len += ksnprintf(&buf[len], sizeof(buf) - len,
                                 "  WiFi: %s", ssid);
            } else {
                len += ksnprintf(&buf[len], sizeof(buf) - len, "  NET: up");
            }
        } else {
            len += ksnprintf(&buf[len], sizeof(buf) - len, "  ETH: up");
        }
    } else {
        len += ksnprintf(&buf[len], sizeof(buf) - len, "  NET: down");
    }

    int text_w = fb_text_width(buf);
    int pill_w = text_w + 32;
    int pill_x = (int)fb_w / 2 - pill_w / 2;
    int pill_y = 16;
    int pill_h = 28;

    fb_draw_rounded(pill_x, pill_y, pill_w, pill_h, 14,
                    UI_NEUTRAL_PILL, UI_NEUTRAL_PILL);
    fb_draw_string(pill_x + 16, pill_y + 6, buf, UI_TEXT_PRIMARY);
}

/* ----- mouse cursor ----- */
static void cursor_render(void) {
    if (!cursor_visible) return;
    int x = cursor_x;
    int y = cursor_y;
    for (int i = 0; i < 12; i++) {
        if (x + i < (int)fb_w) {
            fb_set_pixel(x + i, y, 0xFF000000);
            fb_set_pixel(x + i, y + 1, 0xFFFFFFFF);
        }
        if (y + i < (int)fb_h) {
            fb_set_pixel(x, y + i, 0xFF000000);
            fb_set_pixel(x + 1, y + i, 0xFFFFFFFF);
        }
    }
    for (int i = 0; i < 8; i++) {
        if (x + i < (int)fb_w && y + i < (int)fb_h) {
            fb_set_pixel(x + i, y + i, 0xFF000000);
        }
    }
    fb_set_pixel(x, y, UI_ACCENT);
}

/* ----- FAB ----- */
static int fab_hovered = 0;

static void fab_render(void) {
    int fab_size = 64;
    int fab_x = (int)fb_w - 96 - fab_size;
    int fab_y = (int)fb_h - 96 - fab_size;

    uint64_t now = timer_get_ms();
    /* 2.5s pulse cycle: phase = (now % 2500) * 1000 / 2500 = 0..999 */
    uint32_t phase = (uint32_t)((now % 2500) * 1000 / 2500);
    int scale_milli = 1000 + (40 * isin(phase)) / 1000;  /* 0.96..1.04 */
    int rendered_size = (fab_size * scale_milli) / 1000;

    fb_draw_circle(fab_x + fab_size/2, fab_y + fab_size/2,
                   rendered_size/2,
                   fab_hovered ? UI_ACCENT_HOT : UI_FAB_CORE);
    fb_draw_circle(fab_x + fab_size/2, fab_y + fab_size/2,
                   rendered_size/2 - 6, UI_FAB_SPARK);
    fb_draw_circle(fab_x + fab_size/2, fab_y + fab_size/2,
                   rendered_size/2 - 8, UI_FAB_CORE);

    int cx = fab_x + fab_size/2;
    int cy = fab_y + fab_size/2;
    fb_draw_circle(cx, cy, 12, 0xFF000000);
    fb_draw_circle(cx, cy, 10, UI_FAB_CORE);
    fb_set_pixel(cx, cy, 0xFFFFFFFF);
}

int fab_contains(int x, int y) {
    int fab_size = 64;
    int fab_x = (int)fb_w - 96 - fab_size;
    int fab_y = (int)fb_h - 96 - fab_size;
    int dx = x - (fab_x + fab_size/2);
    int dy = y - (fab_y + fab_size/2);
    return dx * dx + dy * dy <= (fab_size/2) * (fab_size/2);
}

/* ----- widget management ----- */
void compositor_add(struct widget* w) {
    if (n_widgets >= MAX_WIDGETS) return;
    w->z = n_widgets;
    widgets[n_widgets++] = w;
}

void compositor_remove(struct widget* w) {
    for (int i = 0; i < n_widgets; i++) {
        if (widgets[i] == w) {
            for (int j = i; j < n_widgets - 1; j++)
                widgets[j] = widgets[j + 1];
            n_widgets--;
            return;
        }
    }
}

void compositor_bring_to_front(struct widget* w) {
    for (int i = 0; i < n_widgets; i++) {
        widgets[i]->focused = 0;
    }
    w->focused = 1;
    for (int i = 0; i < n_widgets; i++) {
        if (widgets[i] == w) {
            for (int j = i; j < n_widgets - 1; j++)
                widgets[j] = widgets[j + 1];
            widgets[n_widgets - 1] = w;
            break;
        }
    }
}

/* ----- event dispatch ----- */
static struct widget* find_widget_at(int x, int y) {
    for (int i = n_widgets - 1; i >= 0; i--) {
        if (!widgets[i]->visible) continue;
        struct widget* w = widgets[i];
        if (x >= w->x && x < w->x + w->w &&
            y >= w->y && y < w->y + w->h) {
            return w;
        }
    }
    return NULL;
}

static void dispatch_events(void) {
    struct event e;
    /* External: drawer event handler (gui/drawer.c) */
    extern int drawer_handle_event(struct event* e);
    while (input_poll(&e)) {
        /* Drawer gets first crack at FAB clicks, Super key, Esc */
        if (drawer_handle_event(&e)) continue;

        /* Left app drawer gets next crack */
        if (left_drawer_handle_event(&e)) continue;

        /* Pro dock gets next crack at mouse events */
        if (ui_dock_handle_event(&e)) continue;

        if (e.type == EV_MOUSE_MOVE) {
            cursor_x = e.mouse.x;
            cursor_y = e.mouse.y;
            fab_hovered = fab_contains(cursor_x, cursor_y);
            /* Update desktop icon hover state */
            ui_desktop_icons_handle_move(cursor_x, cursor_y);
            if (drag_widget) {
                drag_widget->x = cursor_x - drag_off_x;
                drag_widget->y = cursor_y - drag_off_y;
                if (drag_widget->x < 0) drag_widget->x = 0;
                if (drag_widget->y < 0) drag_widget->y = 0;
                if (drag_widget->x + drag_widget->w > (int)fb_w)
                    drag_widget->x = fb_w - drag_widget->w;
                if (drag_widget->y + drag_widget->h > (int)fb_h)
                    drag_widget->y = fb_h - drag_widget->h;
            }
        } else if (e.type == EV_MOUSE_DOWN) {
            if (fab_contains(cursor_x, cursor_y)) {
                continue;
            }
            /* Top floating bar gets first crack (mic button, search box). */
            if (top_bar_handle_click(cursor_x, cursor_y)) {
                continue;
            }
            struct widget* w = find_widget_at(cursor_x, cursor_y);
            if (w) {
                /* Check if click is on the close button (top-right of title bar) */
                int close_x = w->x + w->w - 24;
                int close_y = w->y + 10;
                if (cursor_x >= close_x && cursor_x < close_x + 16 &&
                    cursor_y >= close_y && cursor_y < close_y + 16) {
                    /* Close button clicked — hide the widget */
                    w->visible = 0;
                    w->focused = 0;
                    continue;
                }

                compositor_bring_to_front(w);
                int title_h = 36;
                if (cursor_y - w->y < title_h && w->draggable) {
                    drag_widget = w;
                    drag_off_x = cursor_x - w->x;
                    drag_off_y = cursor_y - w->y;
                }
                if (w->on_event) w->on_event(w, &e);
            } else {
                /* No widget under cursor — check the new Material app grid
                 * first (it covers most of the desktop), then fall back to
                 * the legacy left-column desktop icons. */
                if (!app_grid_handle_click(cursor_x, cursor_y)) {
                    ui_desktop_icons_handle_click(cursor_x, cursor_y);
                }
            }
        } else if (e.type == EV_MOUSE_UP) {
            drag_widget = NULL;
            struct widget* w = find_widget_at(cursor_x, cursor_y);
            if (w && w->on_event) w->on_event(w, &e);
        } else if (e.type == EV_KEY_DOWN || e.type == EV_KEY_UP) {
            for (int i = n_widgets - 1; i >= 0; i--) {
                if (widgets[i]->visible && widgets[i]->focused && widgets[i]->on_event) {
                    widgets[i]->on_event(widgets[i], &e);
                    break;
                }
            }
        }
    }
}

/* ----- compositor API ----- */
void compositor_init(void) {
    /* Initialize the new top floating bar (animated, with STT) and
     * the Material-Design app grid before the run loop starts. */
    top_bar_init();
    app_grid_init();
}

void compositor_run(void);

/* External: drawer functions (gui/drawer.c) */
extern void drawer_render(void);
extern int  drawer_is_open(void);
extern int  drawer_handle_event(struct event* e);

/* External: desktop icons (gui/desktop_icons.c) */
extern void desktop_icons_render(void);
int desktop_icons_handle_click(int x, int y);

void compositor_run(void) {
    if (!fb_available) {
        pr_warn("compositor: no framebuffer, falling back to shell\n");
        return;
    }

    particles_init();
    compositor_running = 1;

    uint64_t last_ms = timer_get_ms();
    pr_info("compositor: running (fb %ux%u, %d widgets)\n",
            (unsigned)fb_w, (unsigned)fb_h, n_widgets);

    while (compositor_running) {
        dispatch_events();
        particles_update();

        /* Render the real wallpaper image (replaces the old gradient) */
        ui_render_wallpaper();

        /* Render particles on top of wallpaper */
        for (int i = 0; i < NUM_PARTICLES; i++) {
            int alpha = particles[i].life * 128 / particles[i].max_life;
            if (alpha < 0) alpha = 0;
            if (alpha > 128) alpha = 128;
            uint32_t color = ((uint32_t)alpha << 24) | 0x00475569u;
            fb_set_pixel(particles[i].x, particles[i].y,
                         fb_blend(fb_get_pixel(particles[i].x, particles[i].y), color));
        }

        /* Render collapsible left app drawer */
        left_drawer_render();

        /* Render the Material-Design app grid (Writer, Calc, Impress,
         * Kdenlive, OBS, VLC, Browser, Mail, Calendar, Photos, Music,
         * Terminal, AI Lab, Editor, Media, Files, Settings). Each
         * icon is clickable — see app_grid_handle_click. */
        app_grid_render();

        for (int i = 0; i < n_widgets; i++) {
            if (widgets[i]->visible && widgets[i]->draw) {
                widgets[i]->draw(widgets[i]);
            }
        }

        /* Professional status bar */
        ui_render_status_bar();
        fab_render();

        /* Animated top floating bar (overrides the old status pill —
         * rendered on top of widgets so the mic button is always
         * clickable even when a window is maximized). */
        top_bar_render();

        /* Professional dock */
        ui_render_dock();

        /* Mini music player (if playing) */
        ui_render_mini_player();

        /* Notifications (top-right) */
        ui_render_notifications();

        /* Render drawer on top of widgets (below cursor) */
        if (drawer_is_open()) {
            drawer_render();
        }

        cursor_render();
        fb_swap();

        uint64_t now = timer_get_ms();
        if (now - last_ms < 16) {
            while (timer_get_ms() - last_ms < 16) {
                hlt();
            }
        }
        last_ms = timer_get_ms();
    }
}

void compositor_quit(void) {
    compositor_running = 0;
}

void compositor_focus_next(void) {
    /* Stub: cycle focus through widgets. Real impl needs z-order traversal. */
}

void compositor_close_focused(void) {
    /* Stub: remove the currently-focused widget. Real impl needs focus tracking. */
}
