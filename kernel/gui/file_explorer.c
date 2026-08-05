/*
 * Lestra OS - File Explorer
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * A two-pane file manager:
 *   - Left sidebar: quick locations (Home, Documents, tmp, proc, dev)
 *   - Right pane:   file/folder grid for the current directory
 *   - Top address bar showing the current path
 *   - Right-click on a file shows a context menu (Open / Delete / Rename)
 *
 * Backed by the in-kernel VFS (kernel/fs/vfs.c). Paths that don't
 * exist in the VFS fall back to "/" so the UI never crashes.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/vfs.h>
#include <lestra/printk.h>
#include <string.h>

#define FE_W    720
#define FE_H    460
#define FE_TITLE_H 36
#define FE_PAD  8

#define FE_SIDEBAR_W   150
#define FE_ADDR_H      28
#define FE_CTX_MENU_W  120
#define FE_CTX_MENU_H  72
#define FE_MAX_ENTRIES 64
#define FE_NAME_MAX    64

struct fe_entry {
    char name[FE_NAME_MAX];
    int  is_dir;
};

struct fe_state {
    char cwd[MAX_PATH_LEN];
    struct fe_entry entries[FE_MAX_ENTRIES];
    int  n_entries;
    int  selected;
    int  sidebar_selected;
    int  ctx_open;
    int  ctx_x, ctx_y;
    int  ctx_target;     /* index into entries[] */
    int  list_scroll;    /* index of first visible entry in the grid */
};

static struct fe_state fe_state;
static struct widget   fe_widget;

/* ---------- path helpers ---------- */
static void fe_normalize_path(const char* in, char* out, size_t out_sz) {
    /* Copy and trim trailing slashes (except root). */
    strncpy(out, in, out_sz - 1);
    out[out_sz - 1] = '\0';
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') {
        out[--len] = '\0';
    }
}

static void fe_join_path(const char* base, const char* rel, char* out,
                         size_t out_sz) {
    if (rel[0] == '/') {
        fe_normalize_path(rel, out, out_sz);
        return;
    }
    if (strcmp(rel, "..") == 0) {
        fe_normalize_path(base, out, out_sz);
        char* slash = strrchr(out, '/');
        if (slash && slash != out) *slash = '\0';
        else if (slash == out) out[1] = '\0';
        return;
    }
    size_t bl = strlen(base);
    if (bl + 1 + strlen(rel) + 1 > out_sz) {
        strncpy(out, base, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    if (bl > 0 && base[bl - 1] == '/') {
        ksnprintf(out, out_sz, "%s%s", base, rel);
    } else {
        ksnprintf(out, out_sz, "%s/%s", base, rel);
    }
}

/* ---------- directory enumeration ---------- */
static void fe_refresh(struct fe_state* st) {
    st->n_entries = 0;
    st->selected = -1;
    st->list_scroll = 0;   /* new directory: snap back to the top */

    /* vfs_open with O_DIRECTORY on the cwd. */
    int fd = vfs_open(st->cwd, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        /* Fall back to root. */
        if (strcmp(st->cwd, "/") != 0) {
            strncpy(st->cwd, "/", sizeof(st->cwd) - 1);
            st->cwd[sizeof(st->cwd) - 1] = '\0';
            fd = vfs_open(st->cwd, O_RDONLY | O_DIRECTORY);
        }
        if (fd < 0) return;
    }

    struct dirent de;
    int idx = 0;
    while (st->n_entries < FE_MAX_ENTRIES) {
        int rc = vfs_readdir(fd, &de);
        if (rc != 0) break;
        if (de.name[0] == '\0') break;
        if (strcmp(de.name, ".")  == 0) continue;
        if (strcmp(de.name, "..") == 0) continue;
        strncpy(st->entries[st->n_entries].name, de.name, FE_NAME_MAX - 1);
        st->entries[st->n_entries].name[FE_NAME_MAX - 1] = '\0';
        st->entries[st->n_entries].is_dir = (de.type == FT_DIRECTORY);
        st->n_entries++;
        idx++;
    }
    vfs_close(fd);
}

/* ---------- sidebar ---------- */
static const char* fe_sidebar_labels[] = {
    "Home", "Documents", "tmp", "proc", "dev"
};
static const char* fe_sidebar_paths[] = {
    "/", "/docs", "/tmp", "/proc", "/dev"
};
#define FE_SIDEBAR_N (int)(sizeof(fe_sidebar_labels)/sizeof(fe_sidebar_labels[0]))

static void fe_draw_sidebar(struct widget* w) {
    int sx = w->x + FE_PAD;
    int sy = w->y + FE_TITLE_H + FE_ADDR_H + FE_PAD;
    int sw = FE_SIDEBAR_W;
    int sh = w->h - FE_TITLE_H - FE_ADDR_H - 2 * FE_PAD;
    fb_fill_rect(sx, sy, sw, sh, 0xFF0E1422);
    fb_draw_string_small(sx + 8, sy + 6, "Locations", UI_ACCENT_SOFT);
    for (int i = 0; i < FE_SIDEBAR_N; i++) {
        int ry = sy + 28 + i * 24;
        if (i == fe_state.sidebar_selected) {
            fb_fill_rect(sx, ry - 2, sw, 22, 0xFF06B6D4);
        }
        fb_draw_string(sx + 12, ry + 4, fe_sidebar_labels[i],
                       UI_TEXT_PRIMARY);
    }
}

/* ---------- file grid ---------- */

/* Compute grid geometry (cols, rows, cell sizes) shared by draw and
 * hit-test so the two paths never disagree about which cell a click
 * landed in. */
static void fe_grid_geom(struct widget* w, int* gx, int* gy,
                         int* gw, int* gh, int* cols, int* rows) {
    *gx = w->x + FE_PAD + FE_SIDEBAR_W + FE_PAD;
    *gy = w->y + FE_TITLE_H + FE_ADDR_H + FE_PAD;
    *gw = w->w - 2 * FE_PAD - FE_SIDEBAR_W - FE_PAD;
    *gh = w->h - FE_TITLE_H - FE_ADDR_H - 2 * FE_PAD;
    int cell_w = 96, cell_h = 80;
    int c = *gw / cell_w; if (c < 1) c = 1;
    int r = *gh / cell_h; if (r < 1) r = 1;
    *cols = c;
    *rows = r;
    (void)cell_w; (void)cell_h;
}

static void fe_draw_grid(struct widget* w) {
    int gx, gy, gw, gh, cols, rows;
    fe_grid_geom(w, &gx, &gy, &gw, &gh, &cols, &rows);
    int cell_w = 96, cell_h = 80;

    fb_fill_rect(gx, gy, gw, gh, 0xFF0A0C12);

    int visible = cols * rows;
    for (int i = 0; i < visible && fe_state.list_scroll + i < fe_state.n_entries; i++) {
        int entry_idx = fe_state.list_scroll + i;
        int cx = gx + (i % cols) * cell_w + 8;
        int cy = gy + (i / cols) * cell_h + 8;
        if (entry_idx == fe_state.selected) {
            fb_fill_rect(cx - 4, cy - 4, cell_w - 8, cell_h - 8,
                         0xFF06B6D4);
        }
        /* Folder icon: a yellow-ish rectangle. File icon: a slate page. */
        uint32_t icon = fe_state.entries[entry_idx].is_dir ? 0xFFFBBF24 : 0xFF94A3B8;
        fb_draw_rounded(cx + 16, cy, 48, 36, 6, icon, icon);
        /* White "page corner" on files. */
        if (!fe_state.entries[entry_idx].is_dir) {
            fb_fill_rect(cx + 50, cy, 14, 14, 0xFFFFFFFF);
        }
        /* Label (truncated to ~11 chars). */
        char label[12];
        size_t l = strlen(fe_state.entries[entry_idx].name);
        if (l > 11) {
            memcpy(label, fe_state.entries[entry_idx].name, 9);
            label[9] = '.'; label[10] = '.'; label[11] = '\0';
        } else {
            strncpy(label, fe_state.entries[entry_idx].name, sizeof(label) - 1);
            label[sizeof(label) - 1] = '\0';
        }
        fb_draw_string_small(cx, cy + 42, label, UI_TEXT_PRIMARY);
    }

    /* Scroll position indicator (only when there's more than fits). */
    if (fe_state.n_entries > visible) {
        int bar_x = gx + gw - 6;
        int bar_h = gh - 4;
        fb_fill_rect(bar_x, gy + 2, 4, bar_h, 0xFF1E293B);
        int thumb_h = bar_h * visible / fe_state.n_entries;
        if (thumb_h < 8) thumb_h = 8;
        int max_scroll = fe_state.n_entries - visible;
        int thumb_y = gy + 2 + (bar_h - thumb_h) * fe_state.list_scroll / max_scroll;
        fb_fill_rect(bar_x, thumb_y, 4, thumb_h, UI_ACCENT);
    }
}

/* ---------- address bar ---------- */
static void fe_draw_addr(struct widget* w) {
    int ax = w->x + FE_PAD;
    int ay = w->y + FE_TITLE_H + FE_PAD;
    int aw = w->w - 2 * FE_PAD;
    fb_fill_rect(ax, ay, aw, FE_ADDR_H, 0xFF1E293B);
    /* House icon */
    fb_fill_rect(ax + 6, ay + 10, 12, 10, UI_ACCENT);
    fb_draw_line(ax + 6, ay + 10, ax + 12, ay + 4, UI_ACCENT);
    fb_draw_line(ax + 12, ay + 4, ax + 18, ay + 10, UI_ACCENT);
    /* Path */
    fb_draw_string(ax + 28, ay + 8, fe_state.cwd, UI_TEXT_PRIMARY);
}

/* ---------- context menu ---------- */
static void fe_draw_ctx(struct widget* w) {
    if (!fe_state.ctx_open) return;
    int mx = fe_state.ctx_x, my = fe_state.ctx_y;
    /* Clamp to window. */
    if (mx + FE_CTX_MENU_W > w->x + w->w) mx = w->x + w->w - FE_CTX_MENU_W;
    if (my + FE_CTX_MENU_H > w->y + w->h) my = w->y + w->h - FE_CTX_MENU_H;
    fb_draw_rounded(mx, my, FE_CTX_MENU_W, FE_CTX_MENU_H, 6,
                    0xFF0E1422, UI_ACCENT);
    static const char* items[] = {"Open", "Delete", "Rename"};
    for (int i = 0; i < 3; i++) {
        fb_draw_string(mx + 8, my + 4 + i * 22, items[i],
                       UI_TEXT_PRIMARY);
    }
}

/* ---------- main draw ---------- */
static void fe_draw(struct widget* w) {
    extern void ui_draw_card(int x, int y, int w, int h, int focused);
    ui_draw_card(w->x, w->y, w->w, w->h, w->focused);
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, FE_TITLE_H - 1, 0xE00E1422);
    fb_draw_string(w->x + 12, w->y + 10, "File Explorer", UI_TEXT_PRIMARY);
    fb_draw_string(w->x + w->w - 20, w->y + 10, "x", UI_TEXT_MUTED);

    fe_draw_addr(w);
    fe_draw_sidebar(w);
    fe_draw_grid(w);
    fe_draw_ctx(w);
}

/* ---------- event handling ---------- */
static int fe_hit_grid_cell(struct widget* w, int mx, int my, int* idx_out) {
    int gx, gy, gw, gh, cols, rows;
    fe_grid_geom(w, &gx, &gy, &gw, &gh, &cols, &rows);
    int cell_w = 96, cell_h = 80;
    if (mx < gx || mx >= gx + gw || my < gy || my >= gy + gh) return 0;
    int col = (mx - gx) / cell_w;
    int row = (my - gy) / cell_h;
    if (col < 0 || col >= cols || row < 0) return 0;
    int idx = fe_state.list_scroll + row * cols + col;
    if (idx < 0 || idx >= fe_state.n_entries) return 0;
    *idx_out = idx;
    return 1;
}

static void fe_on_event(struct widget* w, struct event* e) {
    if (e->type == EV_MOUSE_SCROLL) {
        /* Wheel up (scroll > 0) views earlier entries, so list_scroll
         * moves toward 0. Wheel down moves toward n_entries. Each tick
         * shifts by one full row (cols entries) to match the visual
         * grid layout. Clamped to [0, max(0, n_entries - visible)]. */
        int gx, gy, gw, gh, cols, rows;
        fe_grid_geom(w, &gx, &gy, &gw, &gh, &cols, &rows);
        int visible = cols * rows;
        int max_scroll = fe_state.n_entries - visible;
        if (max_scroll < 0) max_scroll = 0;
        int delta_entries = e->mouse.scroll * cols;
        int new_scroll = fe_state.list_scroll - delta_entries;
        if (new_scroll < 0) new_scroll = 0;
        if (new_scroll > max_scroll) new_scroll = max_scroll;
        fe_state.list_scroll = new_scroll;
        return;
    }

    if (e->type != EV_MOUSE_DOWN) return;
    int mx = e->mouse.x, my = e->mouse.y;
    /* Close button */
    int cx = w->x + w->w - 24, cy = w->y + 10;
    if (mx >= cx && mx < cx + 16 && my >= cy && my < cy + 16) {
        w->visible = 0;
        return;
    }
    /* If context menu is open, route clicks to it. */
    if (fe_state.ctx_open) {
        if (mx >= fe_state.ctx_x &&
            mx < fe_state.ctx_x + FE_CTX_MENU_W &&
            my >= fe_state.ctx_y &&
            my < fe_state.ctx_y + FE_CTX_MENU_H) {
            int sel = (my - fe_state.ctx_y) / 22;
            if (fe_state.ctx_target >= 0 &&
                fe_state.ctx_target < fe_state.n_entries) {
                struct fe_entry* en = &fe_state.entries[fe_state.ctx_target];
                if (sel == 0) {
                    /* Open */
                    if (en->is_dir) {
                        char np[MAX_PATH_LEN];
                        fe_join_path(fe_state.cwd, en->name, np, sizeof(np));
                        strncpy(fe_state.cwd, np, sizeof(fe_state.cwd) - 1);
                        fe_state.cwd[sizeof(fe_state.cwd) - 1] = '\0';
                        fe_refresh(&fe_state);
                    } else {
                        pr_info("file_explorer: open %s\n", en->name);
                    }
                } else if (sel == 1) {
                    pr_info("file_explorer: delete %s (stub)\n", en->name);
                } else if (sel == 2) {
                    pr_info("file_explorer: rename %s (stub)\n", en->name);
                }
            }
            fe_state.ctx_open = 0;
            return;
        }
        fe_state.ctx_open = 0;  /* click outside closes it */
        return;
    }
    /* Sidebar click? */
    int sx = w->x + FE_PAD;
    int sy = w->y + FE_TITLE_H + FE_ADDR_H + FE_PAD;
    if (mx >= sx && mx < sx + FE_SIDEBAR_W &&
        my >= sy + 24 && my < sy + 24 + FE_SIDEBAR_N * 24) {
        int idx = (my - (sy + 24)) / 24;
        if (idx >= 0 && idx < FE_SIDEBAR_N) {
            fe_state.sidebar_selected = idx;
            strncpy(fe_state.cwd, fe_sidebar_paths[idx],
                    sizeof(fe_state.cwd) - 1);
            fe_state.cwd[sizeof(fe_state.cwd) - 1] = '\0';
            fe_refresh(&fe_state);
        }
        return;
    }
    /* Grid cell click? */
    int idx = -1;
    if (fe_hit_grid_cell(w, mx, my, &idx)) {
        fe_state.selected = idx;
        /* Right-click → context menu. */
        if (e->mouse.buttons & MOUSE_BTN_RIGHT) {
            fe_state.ctx_open = 1;
            fe_state.ctx_x = mx;
            fe_state.ctx_y = my;
            fe_state.ctx_target = idx;
        } else if (fe_state.entries[idx].is_dir) {
            /* Double behaviour: left-click opens folder immediately. */
            char np[MAX_PATH_LEN];
            fe_join_path(fe_state.cwd, fe_state.entries[idx].name,
                         np, sizeof(np));
            strncpy(fe_state.cwd, np, sizeof(fe_state.cwd) - 1);
            fe_state.cwd[sizeof(fe_state.cwd) - 1] = '\0';
            fe_refresh(&fe_state);
        }
        return;
    }
}

/* ---------- public ---------- */
struct widget* file_explorer_create(int x, int y) {
    memset(&fe_state, 0, sizeof(fe_state));
    strncpy(fe_state.cwd, "/", sizeof(fe_state.cwd) - 1);
    fe_state.selected = -1;
    fe_state.sidebar_selected = 0;
    fe_state.list_scroll = 0;
    fe_refresh(&fe_state);

    fe_widget.x = x;
    fe_widget.y = y;
    fe_widget.w = FE_W;
    fe_widget.h = FE_H;
    fe_widget.visible = 1;
    fe_widget.focused = 1;
    fe_widget.draggable = 1;
    fe_widget.resizable = 0;
    fe_widget.draw = fe_draw;
    fe_widget.on_event = fe_on_event;
    fe_widget.state = &fe_state;
    memcpy(fe_widget.title, "File Explorer", 14);
    return &fe_widget;
}
