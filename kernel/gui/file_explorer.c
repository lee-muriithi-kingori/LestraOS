/*
 * Lestra OS - File Explorer
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
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

/* Toast notifications + editor launcher (sibling gui/ modules). */
extern void notify_show(const char* title, const char* body, uint32_t color);
extern struct widget* editor_create(int x, int y);
extern void compositor_add(struct widget* w);
extern void compositor_bring_to_front(struct widget* w);
extern int  editor_load_file(const char* path);

/* vfs_rename() is being added by task W3-B in kernel/fs/vfs.c. It is
 * not yet declared in <lestra/vfs.h>, so we forward-declare it here
 * with the agreed signature. If W3-B's signature changes, only this
 * one line needs updating. */
extern int vfs_rename(const char* oldpath, const char* newpath);

#define FE_COLOR_OK      0xFF16A34A   /* green  - success */
#define FE_COLOR_ERR     0xFFEF4444   /* red    - error   */
#define FE_COLOR_INFO    UI_ACCENT    /* cyan   - info    */

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
    /* Inline rename modal state. When rename_active == 1, the widget
     * captures EV_KEY_DOWN events to build the new filename, draws a
     * centered input box over the grid, and calls vfs_rename() on
     * Enter. Esc cancels. This is a self-contained modal — the rest
     * of the GUI has no input-dialog primitive (kernel/gui/dialogs.c
     * only has About + Help), so the file_explorer rolls its own. */
    int  rename_active;
    int  rename_target;              /* index into entries[] */
    char rename_buf[FE_NAME_MAX];
    int  rename_len;
};

static struct fe_state fe_state;
static struct widget   fe_widget;

/* Singleton editor widget pointer. We lazily create the editor the
 * first time the user asks to Open a file, and reuse the same widget
 * on subsequent Opens (the editor state is a singleton too, see
 * kernel/gui/editor.c). */
static struct widget* fe_editor_w = NULL;

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

/* ---------- inline rename modal ---------- */
/* A small centered input box drawn over the grid when rename_active.
 * The user types a new name; Enter confirms (vfs_rename), Esc cancels.
 * We render a dimmed overlay so the rest of the grid reads as inert
 * while the modal is up. */
#define FE_RENAME_BOX_W  320
#define FE_RENAME_BOX_H  100

static void fe_draw_rename_modal(struct widget* w) {
    if (!fe_state.rename_active) return;

    /* Dim overlay over the whole widget. */
    fb_fill_rect(w->x, w->y + FE_TITLE_H, w->w, w->h - FE_TITLE_H,
                 0x80000000u);

    int bx = w->x + (w->w - FE_RENAME_BOX_W) / 2;
    int by = w->y + (w->h - FE_RENAME_BOX_H) / 2;
    fb_draw_rounded(bx, by, FE_RENAME_BOX_W, FE_RENAME_BOX_H, 8,
                    UI_CARD_BG, UI_ACCENT);

    /* Title. */
    fb_draw_string(bx + 12, by + 10, "Rename to:", UI_TEXT_PRIMARY);

    /* Input field background. */
    int fx = bx + 12, fy = by + 34, fw = FE_RENAME_BOX_W - 24, fh = 24;
    fb_fill_rect(fx, fy, fw, fh, 0xFF0A0C12);
    fb_draw_string(fx + 4, fy + 4, fe_state.rename_buf, UI_TEXT_PRIMARY);

    /* Blinking cursor (toggles every 500ms — matches the editor). */
    extern uint64_t timer_get_ms(void);
    if ((timer_get_ms() / 500) % 2 == 0) {
        int cx = fx + 4 + fe_state.rename_len * 8;
        fb_fill_rect(cx, fy + 2, 8, fh - 4, UI_ACCENT);
    }

    /* Hint line. */
    fb_draw_string_small(bx + 12, by + 68,
                         "Enter: confirm    Esc: cancel",
                         UI_TEXT_MUTED);
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
    fe_draw_rename_modal(w);
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

/* ---------- file actions (Open / Delete / Rename) ----------
 *
 * These were the three stubs flagged in W1-F: Open for a non-dir only
 * pr_info()'d, Delete and Rename were "(stub)". They now drive real
 * VFS operations (vfs_open via editor_load_file, vfs_unlink, and
 * vfs_rename — the latter added by task W3-B) and surface the result
 * as a toast notification (kernel/gui/notifications.c). */

/* Lazily create the editor widget if needed, show it, and load the
 * given path into its buffer. The editor widget is a singleton
 * (single static editor_widget in editor.c) so the same pointer is
 * reused across Opens. */
static void fe_launch_editor_with_file(const char* path) {
    if (!fe_editor_w) {
        fe_editor_w = editor_create(200, 60);
        if (fe_editor_w) compositor_add(fe_editor_w);
    }
    if (!fe_editor_w) return;
    fe_editor_w->visible = 1;
    compositor_bring_to_front(fe_editor_w);

    int rc = editor_load_file(path);
    if (rc == 0) {
        char msg[96];
        ksnprintf(msg, sizeof(msg), "Opened %s", path);
        notify_show("File Explorer", msg, FE_COLOR_OK);
    } else {
        char msg[96];
        ksnprintf(msg, sizeof(msg), "Could not open %s", path);
        notify_show("File Explorer", msg, FE_COLOR_ERR);
    }
}

/* Build the full VFS path for the entry at index `idx` and write it
 * into `out` (NUL-terminated). Returns 0 on success, -1 if idx is out
 * of range or out is too small. */
static int fe_entry_path(int idx, char* out, size_t out_sz) {
    if (idx < 0 || idx >= fe_state.n_entries) return -1;
    fe_join_path(fe_state.cwd, fe_state.entries[idx].name, out, out_sz);
    return 0;
}

/* Delete the entry at fe_state.ctx_target via vfs_unlink(). On
 * success, refresh the listing so the file disappears from the grid. */
static void fe_delete_current_target(void) {
    if (fe_state.ctx_target < 0 || fe_state.ctx_target >= fe_state.n_entries)
        return;
    struct fe_entry* en = &fe_state.entries[fe_state.ctx_target];
    if (en->is_dir) {
        notify_show("File Explorer",
                    "Use 'file rmdir' in shell to delete a folder",
                    FE_COLOR_ERR);
        return;
    }

    char path[MAX_PATH_LEN];
    if (fe_entry_path(fe_state.ctx_target, path, sizeof(path)) < 0) return;

    pr_info("file_explorer: unlink '%s'\n", path);
    int rc = vfs_unlink(path);
    if (rc == 0) {
        char msg[96];
        ksnprintf(msg, sizeof(msg), "Deleted %s", en->name);
        notify_show("File Explorer", msg, FE_COLOR_OK);
        fe_refresh(&fe_state);
    } else {
        char msg[96];
        ksnprintf(msg, sizeof(msg), "Delete failed: %s", en->name);
        notify_show("File Explorer", msg, FE_COLOR_ERR);
    }
}

/* Begin inline rename for the entry at fe_state.ctx_target. Seeds the
 * input buffer with the current name so the user can edit it. */
static void fe_begin_rename(void) {
    if (fe_state.ctx_target < 0 || fe_state.ctx_target >= fe_state.n_entries)
        return;
    struct fe_entry* en = &fe_state.entries[fe_state.ctx_target];
    strncpy(fe_state.rename_buf, en->name, FE_NAME_MAX - 1);
    fe_state.rename_buf[FE_NAME_MAX - 1] = '\0';
    fe_state.rename_len = (int)strlen(fe_state.rename_buf);
    fe_state.rename_target = fe_state.ctx_target;
    fe_state.rename_active = 1;
    pr_info("file_explorer: rename '%s' (editing)\n", en->name);
}

static void fe_cancel_rename(void) {
    fe_state.rename_active = 0;
    fe_state.rename_buf[0] = '\0';
    fe_state.rename_len = 0;
    fe_state.rename_target = -1;
}

/* Confirm the rename: build the new path, call vfs_rename, refresh. */
static void fe_confirm_rename(void) {
    int idx = fe_state.rename_target;
    fe_state.rename_active = 0;

    if (idx < 0 || idx >= fe_state.n_entries) {
        fe_cancel_rename();
        return;
    }
    /* Reject empty / unchanged names. */
    if (fe_state.rename_len == 0) {
        notify_show("File Explorer", "Rename: name is empty",
                    FE_COLOR_ERR);
        fe_cancel_rename();
        return;
    }
    struct fe_entry* en = &fe_state.entries[idx];
    if (strcmp(en->name, fe_state.rename_buf) == 0) {
        /* No change — silently do nothing. */
        fe_cancel_rename();
        return;
    }

    char old_path[MAX_PATH_LEN];
    char new_path[MAX_PATH_LEN];
    if (fe_entry_path(idx, old_path, sizeof(old_path)) < 0) {
        fe_cancel_rename();
        return;
    }
    fe_join_path(fe_state.cwd, fe_state.rename_buf, new_path, sizeof(new_path));

    pr_info("file_explorer: rename '%s' -> '%s'\n", old_path, new_path);
    int rc = vfs_rename(old_path, new_path);
    if (rc == 0) {
        char msg[128];
        ksnprintf(msg, sizeof(msg), "%s -> %s", en->name,
                  fe_state.rename_buf);
        notify_show("File Explorer", msg, FE_COLOR_OK);
        fe_refresh(&fe_state);
    } else {
        char msg[128];
        ksnprintf(msg, sizeof(msg), "Rename failed: %s", en->name);
        notify_show("File Explorer", msg, FE_COLOR_ERR);
    }
    fe_cancel_rename();
}

/* Handle a key while the rename modal is up. Returns 1 if the key was
 * consumed (always — the modal swallows all keys while active). */
static int fe_handle_rename_key(struct event* e) {
    if (!fe_state.rename_active) return 0;
    if (e->type != EV_KEY_DOWN) return 1;  /* swallow non-key events too */

    /* Esc (scancode 0x01) — cancel. keyboard.c pushes ASCII 27 (0x1B)
     * for Esc, so we must drain it to keep the buffer in sync. */
    if (e->key.scancode == KEY_ESC) {
        if (keyboard_has_key()) (void)keyboard_getchar();
        fe_cancel_rename();
        return 1;
    }
    /* Enter — confirm. keyboard.c pushes '\n' for Enter, drain it. */
    if (e->key.scancode == KEY_ENTER) {
        if (keyboard_has_key()) (void)keyboard_getchar();
        fe_confirm_rename();
        return 1;
    }

    /* All other keys come through the keyboard buffer as ASCII. */
    if (!keyboard_has_key()) return 1;
    char c = keyboard_getchar();

    if (c == '\b' || c == 127) {
        if (fe_state.rename_len > 0) {
            fe_state.rename_len--;
            fe_state.rename_buf[fe_state.rename_len] = '\0';
        }
        return 1;
    }
    /* Accept printable ASCII, but reject '/' and '\0' (path separators
     * would let the user "rename" a file into a different directory,
     * which the simple join_path below doesn't handle correctly). */
    if (c >= 0x20 && c < 0x7F && c != '/' &&
        fe_state.rename_len < FE_NAME_MAX - 1) {
        fe_state.rename_buf[fe_state.rename_len++] = c;
        fe_state.rename_buf[fe_state.rename_len] = '\0';
    }
    return 1;
}

static void fe_on_event(struct widget* w, struct event* e) {
    /* Rename modal swallows all input while active. */
    if (fe_state.rename_active) {
        fe_handle_rename_key(e);
        return;
    }

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
                        /* Open the file in the editor (W1-F fix). Build
                         * the full VFS path, lazily create the editor
                         * widget if needed, bring it to front, and
                         * load the file contents into its buffer. */
                        char path[MAX_PATH_LEN];
                        fe_join_path(fe_state.cwd, en->name, path, sizeof(path));
                        fe_launch_editor_with_file(path);
                    }
                } else if (sel == 1) {
                    /* Delete — vfs_unlink + refresh + toast (W1-F fix). */
                    fe_delete_current_target();
                } else if (sel == 2) {
                    /* Rename — open the inline modal (W1-F fix). The
                     * actual vfs_rename happens on Enter. */
                    fe_begin_rename();
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
