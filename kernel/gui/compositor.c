/*
 * Lestra OS - Compositor (clean rewrite)
 * Copyright (c) 2026 lestramk.org
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/keyboard.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <lestra/mm.h>
#include <string.h>

#define MAX_WIDGETS 32
static struct widget* widgets[MAX_WIDGETS];
static int n_widgets = 0;
static int running = 0;

static int cursor_x = 512, cursor_y = 384;
static int cursor_visible = 1;

struct widget* drag_widget = NULL;
static int drag_off_x = 0, drag_off_y = 0;

static uint32_t xorshift = 0x12345678;
static uint32_t rng(void) {
    xorshift ^= xorshift << 13;
    xorshift ^= xorshift >> 17;
    xorshift ^= xorshift << 5;
    return xorshift;
}

#define NUM_PARTICLES 30
struct particle { int x, y, vx, vy, life, max_life; };
static struct particle particles[NUM_PARTICLES];

static void particles_init(void) {
    for (int i = 0; i < NUM_PARTICLES; i++) {
        particles[i].x = rng() % fb_w;
        particles[i].y = rng() % fb_h;
        particles[i].vx = (rng() % 3) - 1;
        particles[i].vy = (rng() % 3) - 1;
        particles[i].life = particles[i].max_life = 100 + (rng() % 200);
    }
}

static void particles_update(void) {
    for (int i = 0; i < NUM_PARTICLES; i++) {
        particles[i].x += particles[i].vx;
        particles[i].y += particles[i].vy;
        particles[i].life--;
        if (particles[i].life <= 0 || particles[i].x < 0 || particles[i].x >= (int)fb_w ||
            particles[i].y < 0 || particles[i].y >= (int)fb_h) {
            particles[i].x = rng() % fb_w;
            particles[i].y = rng() % fb_h;
            particles[i].vx = (rng() % 3) - 1;
            particles[i].vy = (rng() % 3) - 1;
            particles[i].life = particles[i].max_life = 100 + (rng() % 200);
        }
    }
}

void compositor_add(struct widget* w) {
    if (!w) return;
    for (int i = 0; i < n_widgets; i++) if (widgets[i] == w) return;
    if (n_widgets >= MAX_WIDGETS) return;
    w->z = n_widgets;
    widgets[n_widgets++] = w;
}

void compositor_remove(struct widget* w) {
    for (int i = 0; i < n_widgets; i++) {
        if (widgets[i] == w) {
            for (int j = i; j < n_widgets - 1; j++) widgets[j] = widgets[j + 1];
            n_widgets--;
            return;
        }
    }
}

void compositor_bring_to_front(struct widget* w) {
    for (int i = 0; i < n_widgets; i++) widgets[i]->focused = 0;
    w->focused = 1;
    for (int i = 0; i < n_widgets; i++) {
        if (widgets[i] == w) {
            for (int j = i; j < n_widgets - 1; j++) widgets[j] = widgets[j + 1];
            widgets[n_widgets - 1] = w;
            break;
        }
    }
}

static struct widget* find_widget_at(int x, int y) {
    for (int i = n_widgets - 1; i >= 0; i--) {
        if (!widgets[i]->visible) continue;
        struct widget* w = widgets[i];
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) return w;
    }
    return NULL;
}

extern void app_grid_init(void);
extern void app_grid_render(void);
extern int app_grid_handle_click(int x, int y);

extern void ui_draw_card(int x, int y, int w, int h, int focused);
extern void wallpaper_render(void);

static void dispatch_events(void) {
    struct event e;
    while (input_poll(&e)) {
        if (e.type == EV_MOUSE_MOVE) {
            cursor_x = e.mouse.x;
            cursor_y = e.mouse.y;
            if (drag_widget) {
                drag_widget->x = cursor_x - drag_off_x;
                drag_widget->y = cursor_y - drag_off_y;
                if (drag_widget->x < 0) drag_widget->x = 0;
                if (drag_widget->y < 0) drag_widget->y = 0;
                if (drag_widget->x + drag_widget->w > (int)fb_w) drag_widget->x = fb_w - drag_widget->w;
                if (drag_widget->y + drag_widget->h > (int)fb_h) drag_widget->y = fb_h - drag_widget->h;
            }
        } else if (e.type == EV_MOUSE_DOWN) {
            if (app_grid_handle_click(cursor_x, cursor_y)) continue;
            if (e.mouse.buttons & MOUSE_BTN_RIGHT) continue;
            struct widget* w = find_widget_at(cursor_x, cursor_y);
            if (w) {
                int close_x = w->x + w->w - 24, close_y = w->y + 10;
                if (cursor_x >= close_x && cursor_x < close_x + 16 && cursor_y >= close_y && cursor_y < close_y + 16) {
                    w->visible = 0; w->focused = 0; continue;
                }
                compositor_bring_to_front(w);
                if (cursor_y - w->y < 36 && w->draggable) {
                    drag_widget = w;
                    drag_off_x = cursor_x - w->x;
                    drag_off_y = cursor_y - w->y;
                }
                if (w->on_event) w->on_event(w, &e);
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

static void cursor_render(void) {
    if (!cursor_visible) return;
    int x = cursor_x, y = cursor_y;
    for (int i = 0; i < 12; i++) {
        if (x + i < (int)fb_w) { fb_set_pixel(x + i, y, 0xFF000000); fb_set_pixel(x + i, y + 1, 0xFFFFFFFF); }
        if (y + i < (int)fb_h) { fb_set_pixel(x, y + i, 0xFF000000); fb_set_pixel(x + 1, y + i, 0xFFFFFFFF); }
    }
    for (int i = 0; i < 8; i++) if (x + i < (int)fb_w && y + i < (int)fb_h) fb_set_pixel(x + i, y + i, 0xFF000000);
    fb_set_pixel(x, y, 0xFF22D3EE);
}

void compositor_init(void) {
    app_grid_init();
}

void compositor_run(void) {
    if (!fb_available) { pr_warn("compositor: no framebuffer\n"); return; }
    particles_init();
    running = 1;
    uint64_t last = timer_get_ms();
    pr_info("compositor: running %ux%u\n", fb_w, fb_h);

    while (running) {
        dispatch_events();
        particles_update();

        wallpaper_render();

        for (int i = 0; i < NUM_PARTICLES; i++) {
            int alpha = particles[i].life * 128 / particles[i].max_life;
            if (alpha < 0) alpha = 0;
            if (alpha > 128) alpha = 128;
            uint32_t c = ((uint32_t)alpha << 24) | 0x00475569;
            fb_set_pixel(particles[i].x, particles[i].y, fb_blend(fb_get_pixel(particles[i].x, particles[i].y), c));
        }

        app_grid_render();

        for (int i = 0; i < n_widgets; i++) {
            if (widgets[i]->visible && widgets[i]->draw) widgets[i]->draw(widgets[i]);
        }

        cursor_render();
        fb_swap();

        uint64_t now = timer_get_ms();
        if (now - last < 16) while (timer_get_ms() - last < 16) hlt();
        last = timer_get_ms();
    }
}

void compositor_quit(void) { running = 0; }

/* ---- Missing symbols for other modules ---- */
static const int16_t sin_t[256] = {
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

int isin(uint32_t phase) {
    return sin_t[(phase * 256 / 1000) & 0xFF];
}

int fab_contains(int x, int y) {
    int fs = 64;
    int fx = (int)fb_w - 96 - fs;
    int fy = (int)fb_h - 96 - fs;
    int dx = x - (fx + fs/2);
    int dy = y - (fy + fs/2);
    return dx*dx + dy*dy <= (fs/2)*(fs/2);
}

struct widget* compositor_get_focused(void) {
    for (int i = n_widgets-1; i >= 0; i--)
        if (widgets[i]->visible && widgets[i]->focused) return widgets[i];
    return NULL;
}

void compositor_focus_next(void) {
    if (n_widgets == 0) return;
    for (int i = 1; i <= n_widgets; i++) {
        int idx = (n_widgets - 1 + i) % n_widgets;
        if (widgets[idx]->visible) {
            compositor_bring_to_front(widgets[idx]);
            break;
        }
    }
}

void compositor_close_focused(void) {
    for (int i = n_widgets-1; i >= 0; i--) {
        if (widgets[i]->visible && widgets[i]->focused) {
            widgets[i]->visible = 0;
            break;
        }
    }
}
