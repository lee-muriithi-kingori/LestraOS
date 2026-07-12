/*
 * Lestra OS - Task Manager
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * A small process-monitoring window. It polls the scheduler for the
 * current process list, renders a 5-column table (PID / Name / State /
 * CPU% / Mem) and shows a 60-sample CPU graph plus a RAM usage bar at
 * the top. An "End Task" button at the bottom kills the currently
 * selected row (if the scheduler exposes a kill API).
 *
 * Because the kernel scheduler is still tiny (only a few built-in
 * tasks), we synthesise the row data here from a small static array
 * so the UI is always demonstrable even when the live process list
 * is empty. A real implementation would call sched_enumerate().
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <lestra/mm.h>
#include <lestra/printk.h>
#include <string.h>

#define TM_W    640
#define TM_H    460
#define TM_TITLE_H 36
#define TM_PAD  8

#define TM_GRAPH_SAMPLES  60
#define TM_MAX_PROCS      16

struct tm_proc {
    int   pid;
    char  name[24];
    char  state[8];   /* "Run", "Slp", "Zmb", "Stp" */
    int   cpu_pct;    /* 0..100 */
    int   mem_kb;
};

struct tm_state {
    struct tm_proc procs[TM_MAX_PROCS];
    int n_procs;
    int selected;
    int cpu_history[TM_GRAPH_SAMPLES];
    int history_head;
    uint64_t last_sample_ms;
};

static struct tm_state tm_state;
static struct widget   tm_widget;

/* ---------- helpers ---------- */
static void tm_sample_processes(void) {
    /* Pull the current free/used memory once per refresh. */
    uintptr_t total = pmm_get_total();
    uintptr_t used  = pmm_get_used();
    int mem_pct = (total > 0) ? (int)((used * 100) / total) : 0;

    /* A real implementation would iterate sched_proc_table here. We
     * synthesise the same five built-in tasks each tick so the UI
     * always shows something interesting. CPU% jitters a little. */
    uint64_t now = timer_get_ms();
    int jitter = (int)((now / 250) % 7);

    static const char* knames[TM_MAX_PROCS] = {
        "init", "shell", "compositor", "netd", "e1000-irq",
        "rtc-tick", "audio-mixer", "ai-engine", "kworker", "vmm-coalesce"
    };
    static const char* kstates[TM_MAX_PROCS] = {
        "Run", "Run", "Run", "Slp", "Slp",
        "Slp", "Slp", "Run", "Slp", "Slp"
    };

    tm_state.n_procs = 10;
    for (int i = 0; i < tm_state.n_procs; i++) {
        tm_state.procs[i].pid = i + 1;
        strncpy(tm_state.procs[i].name, knames[i],
                sizeof(tm_state.procs[i].name) - 1);
        tm_state.procs[i].name[sizeof(tm_state.procs[i].name) - 1] = '\0';
        strncpy(tm_state.procs[i].state, kstates[i],
                sizeof(tm_state.procs[i].state) - 1);
        tm_state.procs[i].state[sizeof(tm_state.procs[i].state) - 1] = '\0';
        /* Different baseline CPU per task + jitter. */
        int base = (i == 0) ? 1 : (i == 1) ? 3 : (i == 2) ? 8 :
                   (i == 7) ? 12 : 0;
        int v = base + ((jitter + i) % 5);
        if (v > 100) v = 100;
        tm_state.procs[i].cpu_pct = v;
        /* Memory grows with PID. */
        tm_state.procs[i].mem_kb = 256 + i * 384 + (mem_pct * 16);
    }
}

static void tm_push_cpu_sample(void) {
    int total = 0;
    for (int i = 0; i < tm_state.n_procs; i++) {
        total += tm_state.procs[i].cpu_pct;
    }
    if (total > 100) total = 100;
    tm_state.cpu_history[tm_state.history_head] = total;
    tm_state.history_head = (tm_state.history_head + 1) % TM_GRAPH_SAMPLES;
}

static void tm_draw_graph(int x, int y, int w, int h) {
    /* Background */
    fb_fill_rect(x, y, w, h, 0xFF05060A);
    fb_draw_rect(x, y, w, h, UI_CARD_BORDER);

    /* Horizontal grid lines */
    for (int g = 1; g < 4; g++) {
        int gy = y + (h * g) / 4;
        for (int gx = x; gx < x + w; gx += 4) {
            fb_set_pixel(gx, gy, 0xFF1E293B);
        }
    }

    /* Plot samples in order, oldest first. */
    int n = TM_GRAPH_SAMPLES;
    int start = tm_state.history_head;
    int prev_x = x, prev_y = y + h;
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % n;
        int v = tm_state.cpu_history[idx];
        int px = x + (w * i) / n;
        int py = y + h - (v * h) / 100;
        if (i > 0) {
            fb_draw_line(prev_x, prev_y, px, py, UI_ACCENT);
        }
        prev_x = px;
        prev_y = py;
    }

    /* Label */
    fb_draw_string_small(x + 4, y + 2, "CPU %", UI_TEXT_MUTED);
}

static void tm_draw_ram_bar(int x, int y, int w, int h) {
    uintptr_t total = pmm_get_total();
    uintptr_t used  = pmm_get_used();
    int pct = (total > 0) ? (int)((used * 100) / total) : 0;

    fb_fill_rect(x, y, w, h, 0xFF05060A);
    fb_draw_rect(x, y, w, h, UI_CARD_BORDER);
    int fw = ((w - 2) * pct) / 100;
    if (fw > 0) {
        fb_fill_rect(x + 1, y + 1, fw, h - 2, UI_ACCENT);
    }
    char lbl[64];
    ksnprintf(lbl, sizeof(lbl),
              "RAM %u MB / %u MB  (%u%%)",
              (unsigned)(used  / (1024 * 1024)),
              (unsigned)(total / (1024 * 1024)),
              (unsigned)pct);
    fb_draw_string_small(x + 6, y + 2, lbl, UI_TEXT_PRIMARY);
}

/* ---------- widget callbacks ---------- */
static void tm_draw(struct widget* w) {
    extern void ui_draw_card(int x, int y, int w, int h, int focused);
    ui_draw_card(w->x, w->y, w->w, w->h, w->focused);

    /* Title bar */
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, TM_TITLE_H - 1, 0xE00E1422);
    fb_draw_string(w->x + 12, w->y + 10, "Task Manager", UI_TEXT_PRIMARY);
    fb_draw_string(w->x + w->w - 20, w->y + 10, "x", UI_TEXT_MUTED);

    /* Sample every 250 ms. */
    uint64_t now = timer_get_ms();
    if (now - tm_state.last_sample_ms > 250) {
        tm_state.last_sample_ms = now;
        tm_sample_processes();
        tm_push_cpu_sample();
    }

    /* Top: CPU graph + RAM bar side by side */
    int top_y = w->y + TM_TITLE_H + TM_PAD;
    int top_h = 70;
    tm_draw_graph(w->x + TM_PAD, top_y,
                  w->w / 2 - TM_PAD - 4, top_h);
    tm_draw_ram_bar(w->x + w->w / 2 + 4, top_y,
                    w->w / 2 - TM_PAD - 4, top_h);

    /* Process table header */
    int tbl_y = top_y + top_h + TM_PAD;
    int cols_x[5];
    int cols_w[5];
    int tbl_x = w->x + TM_PAD;
    int tbl_w = w->w - 2 * TM_PAD;
    /* PID / Name / State / CPU% / Mem */
    cols_w[0] = 50;  cols_w[1] = tbl_w - 50 - 60 - 60 - 80;
    cols_w[2] = 60;  cols_w[3] = 60;  cols_w[4] = 80;
    cols_x[0] = tbl_x;
    cols_x[1] = cols_x[0] + cols_w[0];
    cols_x[2] = cols_x[1] + cols_w[1];
    cols_x[3] = cols_x[2] + cols_w[2];
    cols_x[4] = cols_x[3] + cols_w[3];

    fb_fill_rect(tbl_x, tbl_y, tbl_w, 18, 0xFF0E1422);
    static const char* hdrs[5] = {"PID","Name","State","CPU%","Mem"};
    for (int c = 0; c < 5; c++) {
        fb_draw_string_small(cols_x[c] + 4, tbl_y + 2, hdrs[c],
                             UI_ACCENT_SOFT);
    }

    /* Rows */
    int row_h = 18;
    int max_visible = (w->y + w->h - 40 - tbl_y - 18) / row_h;
    if (max_visible < 0) max_visible = 0;
    if (max_visible > tm_state.n_procs) max_visible = tm_state.n_procs;
    for (int i = 0; i < max_visible; i++) {
        int ry = tbl_y + 18 + i * row_h;
        if (i == tm_state.selected) {
            fb_fill_rect(tbl_x, ry, tbl_w, row_h, 0xFF06B6D4);
        } else if (i % 2 == 0) {
            fb_fill_rect(tbl_x, ry, tbl_w, row_h, 0xFF111827);
        }
        char buf[16];
        ksnprintf(buf, sizeof(buf), "%d", tm_state.procs[i].pid);
        fb_draw_string_small(cols_x[0] + 4, ry + 2, buf, UI_TEXT_PRIMARY);
        fb_draw_string_small(cols_x[1] + 4, ry + 2,
                             tm_state.procs[i].name, UI_TEXT_PRIMARY);
        fb_draw_string_small(cols_x[2] + 4, ry + 2,
                             tm_state.procs[i].state, UI_TEXT_MUTED);
        ksnprintf(buf, sizeof(buf), "%u%%",
                  (unsigned)tm_state.procs[i].cpu_pct);
        fb_draw_string_small(cols_x[3] + 4, ry + 2, buf,
                             (tm_state.procs[i].cpu_pct > 50)
                                ? UI_DANGER : UI_TEXT_PRIMARY);
        ksnprintf(buf, sizeof(buf), "%uK",
                  (unsigned)tm_state.procs[i].mem_kb);
        fb_draw_string_small(cols_x[4] + 4, ry + 2, buf, UI_TEXT_PRIMARY);
    }

    /* End Task button */
    int btn_w = 100, btn_h = 24;
    int btn_x = w->x + w->w - btn_w - TM_PAD;
    int btn_y = w->y + w->h - btn_h - TM_PAD;
    uint32_t bg = (tm_state.selected >= 0) ? UI_DANGER : UI_TEXT_FAINT;
    fb_draw_rounded(btn_x, btn_y, btn_w, btn_h, 6, bg, bg);
    fb_draw_string(btn_x + 14, btn_y + 6, "End Task", UI_TEXT_PRIMARY);
}

static int tm_hit_end_task_button(struct widget* w, int mx, int my) {
    int btn_w = 100, btn_h = 24;
    int btn_x = w->x + w->w - btn_w - TM_PAD;
    int btn_y = w->y + w->h - btn_h - TM_PAD;
    return (mx >= btn_x && mx < btn_x + btn_w &&
            my >= btn_y && my < btn_y + btn_h);
}

static void tm_on_event(struct widget* w, struct event* e) {
    if (e->type == EV_MOUSE_DOWN) {
        /* Close button */
        int cx = w->x + w->w - 24, cy = w->y + 10;
        if (e->mouse.x >= cx && e->mouse.x < cx + 16 &&
            e->mouse.y >= cy && e->mouse.y < cy + 16) {
            w->visible = 0;
            return;
        }
        /* End Task button */
        if (tm_hit_end_task_button(w, e->mouse.x, e->mouse.y)) {
            if (tm_state.selected >= 0 &&
                tm_state.selected < tm_state.n_procs) {
                pr_info("task_manager: killing PID %d (%s)\n",
                        tm_state.procs[tm_state.selected].pid,
                        tm_state.procs[tm_state.selected].name);
                /* In a real OS we'd call sched_kill(pid) here. */
            }
            return;
        }
        /* Table row click? */
        int top_y = w->y + TM_TITLE_H + TM_PAD;
        int top_h = 70;
        int tbl_y = top_y + top_h + TM_PAD + 18;
        int row_h = 18;
        int tbl_x = w->x + TM_PAD;
        int tbl_w = w->w - 2 * TM_PAD;
        if (e->mouse.x >= tbl_x && e->mouse.x < tbl_x + tbl_w &&
            e->mouse.y >= tbl_y) {
            int idx = (e->mouse.y - tbl_y) / row_h;
            if (idx >= 0 && idx < tm_state.n_procs) {
                tm_state.selected = idx;
            }
        }
    }
}

/* ---------- public ---------- */
struct widget* task_manager_create(int x, int y) {
    memset(&tm_state, 0, sizeof(tm_state));
    tm_state.selected = -1;
    tm_state.last_sample_ms = 0;
    tm_sample_processes();
    /* Pre-fill the graph with the current sample. */
    for (int i = 0; i < TM_GRAPH_SAMPLES; i++) {
        tm_push_cpu_sample();
    }

    tm_widget.x = x;
    tm_widget.y = y;
    tm_widget.w = TM_W;
    tm_widget.h = TM_H;
    tm_widget.visible = 1;
    tm_widget.focused = 1;
    tm_widget.draggable = 1;
    tm_widget.resizable = 0;
    tm_widget.draw = tm_draw;
    tm_widget.on_event = tm_on_event;
    tm_widget.state = &tm_state;
    memcpy(tm_widget.title, "Task Manager", 13);
    return &tm_widget;
}
