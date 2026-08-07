/*
 * Lestra OS - Pro Editor with syntax highlighting
 * Copyright (c) 2026 lestramk.org / Lee Muriithi Kingori
 *
 * A small text editor that knows about C syntax. It reuses the line-
 * buffer layout from editor.c but adds:
 *   - Line numbers in a wider gutter
 *   - C syntax highlighting (keywords, strings, comments, numbers,
 *     preprocessor directives)
 *   - Ctrl+S to save (via VFS)
 *   - Ctrl+F to open an inline search box; Enter jumps to the next
 *     match starting from the cursor.
 *
 * Tokenizer is intentionally simple: a single linear scan with
 * state transitions for "in comment", "in string", "in identifier",
 * etc. No preprocessing, no macros beyond what the lexer sees.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/vfs.h>
#include <lestra/timer.h>
#include <lestra/printk.h>
#include <string.h>

#define EP_W    720
#define EP_H    480
#define EP_TITLE_H 36
#define EP_PAD  8
#define EP_STATUS_H 24
#define EP_SEARCH_H 28
#define EP_GUTTER_W 48
#define CHAR_W 8
#define CHAR_H 16
#define EP_COLS ((EP_W - 2 * EP_PAD - EP_GUTTER_W) / CHAR_W)
#define EP_ROWS ((EP_H - EP_TITLE_H - EP_STATUS_H - 2 * EP_PAD - EP_SEARCH_H) / CHAR_H)
#define EP_MAX_LINES 512
#define EP_MAX_LINE_LEN 200

/* Token kinds for the highlighter */
enum {
    HL_TEXT = 0,
    HL_KEYWORD,
    HL_STRING,
    HL_COMMENT,
    HL_NUMBER,
    HL_PREPROC,
    HL_TYPE,
};

struct ep_line {
    char text[EP_MAX_LINE_LEN];
    int  len;
};

struct ep_state {
    struct ep_line lines[EP_MAX_LINES];
    int n_lines;
    int cursor_col;
    int cursor_row;
    int scroll_row;
    char filename[64];
    int  active;
    int  dirty;
    /* Search box */
    int  search_open;
    char search_buf[48];
    int  search_len;
    int  search_match_row;
    int  search_match_col;
};

static struct ep_state ep_state;
static struct widget   ep_widget;

/* ---------- keyword tables ---------- */
static const char* ep_keywords[] = {
    "auto","break","case","const","continue","default","do","else",
    "enum","extern","for","goto","if","inline","register","restrict",
    "return","sizeof","static","struct","switch","typedef","union",
    "volatile","while","_Bool","_Complex","_Imaginary",
    NULL
};
static const char* ep_types[] = {
    "void","char","short","int","long","float","double","signed",
    "unsigned","size_t","ssize_t","uint8_t","uint16_t","uint32_t",
    "uint64_t","int8_t","int16_t","int32_t","int64_t","bool",
    NULL
};

static int ep_is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int ep_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int ep_match_word(const char* p, int len, const char** table) {
    for (int i = 0; table[i]; i++) {
        const char* w = table[i];
        size_t wl = strlen(w);
        if ((int)wl == len && strncmp(p, w, wl) == 0) return 1;
    }
    return 0;
}

/* Highlight a single line: writes the token kind for each char into
 * `kinds[]` (length len). */
static void ep_highlight_line(const char* text, int len, uint8_t* kinds) {
    int i = 0;
    int in_block_comment_from_prev = 0;  /* not used across lines here */
    (void)in_block_comment_from_prev;
    while (i < len) {
        char c = text[i];
        /* Line comment */
        if (c == '/' && i + 1 < len && text[i + 1] == '/') {
            for (int k = i; k < len; k++) kinds[k] = HL_COMMENT;
            return;
        }
        /* Block comment start */
        if (c == '/' && i + 1 < len && text[i + 1] == '*') {
            int j = i + 2;
            while (j < len) {
                if (text[j] == '*' && j + 1 < len && text[j + 1] == '/') {
                    j += 2; break;
                }
                j++;
            }
            for (int k = i; k < j && k < len; k++) kinds[k] = HL_COMMENT;
            i = j;
            continue;
        }
        /* String */
        if (c == '"') {
            int j = i + 1;
            while (j < len && text[j] != '"') {
                if (text[j] == '\\' && j + 1 < len) j++;
                j++;
            }
            if (j < len) j++;
            for (int k = i; k < j && k < len; k++) kinds[k] = HL_STRING;
            i = j;
            continue;
        }
        /* Char literal */
        if (c == '\'') {
            int j = i + 1;
            while (j < len && text[j] != '\'') {
                if (text[j] == '\\' && j + 1 < len) j++;
                j++;
            }
            if (j < len) j++;
            for (int k = i; k < j && k < len; k++) kinds[k] = HL_STRING;
            i = j;
            continue;
        }
        /* Preprocessor */
        if (c == '#') {
            int j = i;
            while (j < len && text[j] != ' ' && text[j] != '\t') j++;
            for (int k = i; k < j && k < len; k++) kinds[k] = HL_PREPROC;
            i = j;
            continue;
        }
        /* Number */
        if (c >= '0' && c <= '9') {
            int j = i;
            while (j < len && (ep_is_ident_char(text[j]) || text[j] == '.')) j++;
            for (int k = i; k < j && k < len; k++) kinds[k] = HL_NUMBER;
            i = j;
            continue;
        }
        /* Identifier / keyword */
        if (ep_is_alpha(c)) {
            int j = i;
            while (j < len && ep_is_ident_char(text[j])) j++;
            int wlen = j - i;
            int kind = HL_TEXT;
            if (ep_match_word(text + i, wlen, ep_keywords)) kind = HL_KEYWORD;
            else if (ep_match_word(text + i, wlen, ep_types)) kind = HL_TYPE;
            for (int k = i; k < j && k < len; k++) kinds[k] = (uint8_t)kind;
            i = j;
            continue;
        }
        kinds[i] = HL_TEXT;
        i++;
    }
}

static uint32_t ep_color_for_kind(uint8_t k) {
    switch (k) {
        case HL_KEYWORD: return 0xFFC084FC;  /* purple */
        case HL_TYPE:    return 0xFF67E8F9;  /* cyan */
        case HL_STRING:  return 0xFF4ADE80;  /* green */
        case HL_COMMENT: return 0xFF64748B;  /* slate */
        case HL_NUMBER:  return 0xFFFBBF24;  /* amber */
        case HL_PREPROC: return 0xFFF87171;  /* red */
        default:         return UI_TEXT_PRIMARY;
    }
}

/* ---------- line management ---------- */
static void ep_insert_char(struct ep_state* st, char c) {
    if (st->cursor_row >= st->n_lines) return;
    struct ep_line* ln = &st->lines[st->cursor_row];
    if (ln->len >= EP_MAX_LINE_LEN - 1) return;
    for (int i = ln->len; i > st->cursor_col; i--) {
        ln->text[i] = ln->text[i - 1];
    }
    ln->text[st->cursor_col] = c;
    ln->len++;
    st->cursor_col++;
    st->dirty = 1;
}

static void ep_new_line(struct ep_state* st) {
    if (st->n_lines >= EP_MAX_LINES) return;
    for (int i = st->n_lines; i > st->cursor_row + 1; i--) {
        memcpy(&st->lines[i], &st->lines[i - 1], sizeof(struct ep_line));
    }
    struct ep_line* cur = &st->lines[st->cursor_row];
    struct ep_line* nxt = &st->lines[st->cursor_row + 1];
    int tail = cur->len - st->cursor_col;
    if (tail > 0) {
        memcpy(nxt->text, cur->text + st->cursor_col, tail);
        nxt->len = tail;
        cur->len = st->cursor_col;
    } else {
        nxt->len = 0;
    }
    st->n_lines++;
    st->cursor_row++;
    st->cursor_col = 0;
    st->dirty = 1;
}

static void ep_backspace(struct ep_state* st) {
    if (st->cursor_col > 0) {
        struct ep_line* ln = &st->lines[st->cursor_row];
        for (int i = st->cursor_col - 1; i < ln->len; i++) {
            ln->text[i] = ln->text[i + 1];
        }
        ln->len--;
        st->cursor_col--;
        st->dirty = 1;
    } else if (st->cursor_row > 0) {
        struct ep_line* prev = &st->lines[st->cursor_row - 1];
        struct ep_line* cur  = &st->lines[st->cursor_row];
        int prev_len = prev->len;
        int cur_len  = cur->len;
        if (prev_len + cur_len < EP_MAX_LINE_LEN) {
            memcpy(prev->text + prev_len, cur->text, cur_len);
            prev->len = prev_len + cur_len;
        }
        for (int i = st->cursor_row; i < st->n_lines - 1; i++) {
            memcpy(&st->lines[i], &st->lines[i + 1], sizeof(struct ep_line));
        }
        st->n_lines--;
        st->cursor_row--;
        st->cursor_col = prev_len;
        st->dirty = 1;
    }
}

static void ep_save(struct ep_state* st) {
    int fd = vfs_open(st->filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        pr_warn("editor_pro: cannot open %s for write\n", st->filename);
        return;
    }
    for (int i = 0; i < st->n_lines; i++) {
        vfs_write(fd, st->lines[i].text, st->lines[i].len);
        vfs_write(fd, "\n", 1);
    }
    vfs_close(fd);
    st->dirty = 0;
    pr_info("editor_pro: saved %s (%d lines)\n", st->filename, st->n_lines);
}

static void ep_search_next(struct ep_state* st) {
    if (st->search_len == 0) return;
    st->search_buf[st->search_len] = '\0';
    int start_row = st->cursor_row;
    int start_col = st->cursor_col + 1;
    for (int r = start_row; r < st->n_lines; r++) {
        struct ep_line* ln = &st->lines[r];
        int from = (r == start_row) ? start_col : 0;
        if (from > ln->len) continue;
        const char* p = strstr(ln->text + from, st->search_buf);
        if (p) {
            st->search_match_row = r;
            st->search_match_col = (int)(p - ln->text);
            st->cursor_row = r;
            st->cursor_col = st->search_match_col + st->search_len;
            /* Scroll into view. */
            if (st->cursor_row < st->scroll_row) {
                st->scroll_row = st->cursor_row;
            } else if (st->cursor_row - st->scroll_row >= EP_ROWS) {
                st->scroll_row = st->cursor_row - EP_ROWS + 1;
            }
            return;
        }
    }
    /* Wrap around. */
    for (int r = 0; r <= start_row && r < st->n_lines; r++) {
        struct ep_line* ln = &st->lines[r];
        const char* p = strstr(ln->text, st->search_buf);
        if (p) {
            st->search_match_row = r;
            st->search_match_col = (int)(p - ln->text);
            st->cursor_row = r;
            st->cursor_col = st->search_match_col + st->search_len;
            return;
        }
    }
}

/* ---------- draw ---------- */
static void ep_draw(struct widget* w) {
    struct ep_state* st = (struct ep_state*)w->state;
    extern void ui_draw_card(int x, int y, int w, int h, int focused);
    ui_draw_card(w->x, w->y, w->w, w->h, w->focused);
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, EP_TITLE_H - 1, 0xE00E1422);
    char title[96];
    ksnprintf(title, sizeof(title), "Editor Pro - %s%s",
              st->filename, st->dirty ? " *" : "");
    fb_draw_string(w->x + 12, w->y + 10, title, UI_TEXT_PRIMARY);
    fb_draw_string(w->x + w->w - 20, w->y + 10, "x", UI_TEXT_MUTED);

    /* Body */
    int bx = w->x + EP_PAD;
    int by = w->y + EP_TITLE_H + EP_PAD;
    int bw = w->w - 2 * EP_PAD;
    int bh = w->h - EP_TITLE_H - EP_STATUS_H - 2 * EP_PAD - EP_SEARCH_H;
    fb_fill_rect(bx, by, bw, bh, 0xFF0A0C12);
    /* Gutter */
    fb_fill_rect(bx, by, EP_GUTTER_W, bh, 0xFF050608);

    for (int i = 0; i < EP_ROWS && st->scroll_row + i < st->n_lines; i++) {
        int row = st->scroll_row + i;
        int y = by + i * CHAR_H;
        /* Line number */
        char num[8];
        ksnprintf(num, sizeof(num), "%4d", row + 1);
        fb_draw_string(bx + 4, y, num,
                       (row == st->cursor_row) ? UI_ACCENT : UI_TEXT_FAINT);
        /* Highlight */
        struct ep_line* ln = &st->lines[row];
        static uint8_t kinds[EP_MAX_LINE_LEN];
        ep_highlight_line(ln->text, ln->len, kinds);
        for (int c = 0; c < ln->len && c < EP_COLS; c++) {
            fb_draw_char(bx + EP_GUTTER_W + 4 + c * CHAR_W, y,
                         ln->text[c], ep_color_for_kind(kinds[c]));
        }
    }

    /* Cursor */
    uint64_t now = timer_get_ms();
    if (st->active && (now / 500) % 2 == 0) {
        int cr = st->cursor_row - st->scroll_row;
        if (cr >= 0 && cr < EP_ROWS) {
            int cx = bx + EP_GUTTER_W + 4 + st->cursor_col * CHAR_W;
            int cy = by + cr * CHAR_H;
            fb_fill_rect(cx, cy, 2, CHAR_H, UI_ACCENT);
        }
    }

    /* Search box (always reserved space; only visible when open). */
    int sy = by + bh + EP_PAD;
    fb_fill_rect(bx, sy, bw, EP_SEARCH_H, 0xFF1E293B);
    if (st->search_open) {
        fb_draw_string(bx + 8, sy + 8, "Find:", UI_ACCENT_SOFT);
        fb_draw_string(bx + 56, sy + 8, st->search_buf, UI_TEXT_PRIMARY);
        /* Cursor */
        if ((now / 500) % 2 == 0) {
            int cx = bx + 56 + st->search_len * CHAR_W;
            fb_fill_rect(cx, sy + 6, 2, CHAR_H, UI_ACCENT);
        }
        fb_draw_string(bx + bw - 200, sy + 8,
                       "Enter=next  Esc=close", UI_TEXT_MUTED);
    } else {
        fb_draw_string(bx + 8, sy + 8,
                       "Ctrl+F: find    Ctrl+S: save",
                       UI_TEXT_MUTED);
    }

    /* Status bar */
    int status_y = w->y + w->h - EP_STATUS_H;
    fb_fill_rect(bx, status_y, bw, EP_STATUS_H - 4, 0xFF0E1422);
    char status[160];
    ksnprintf(status, sizeof(status),
              "Ln %d, Col %d   %d lines   %s   C syntax",
              st->cursor_row + 1, st->cursor_col + 1, st->n_lines,
              st->dirty ? "modified" : "saved");
    fb_draw_string(bx + 6, status_y + 4, status, UI_TEXT_MUTED);
}

/* ---------- events ---------- */
static void ep_on_event(struct widget* w, struct event* e) {
    struct ep_state* st = (struct ep_state*)w->state;
    if (e->type == EV_MOUSE_DOWN) {
        int mx = e->mouse.x, my = e->mouse.y;
        int cx = w->x + w->w - 24, cy = w->y + 10;
        if (mx >= cx && mx < cx + 16 && my >= cy && my < cy + 16) {
            w->visible = 0;
            return;
        }
        st->active = 1;
        /* Click in body: place cursor. */
        int bx = w->x + EP_PAD;
        int by = w->y + EP_TITLE_H + EP_PAD;
        int bh = w->h - EP_TITLE_H - EP_STATUS_H - 2 * EP_PAD - EP_SEARCH_H;
        if (mx >= bx + EP_GUTTER_W && my >= by && my < by + bh) {
            int r = (my - by) / CHAR_H + st->scroll_row;
            int c = (mx - bx - EP_GUTTER_W - 4) / CHAR_W;
            if (r >= 0 && r < st->n_lines) {
                st->cursor_row = r;
                st->cursor_col = (c >= 0) ? c : 0;
                if (st->cursor_col > st->lines[r].len)
                    st->cursor_col = st->lines[r].len;
            }
        }
        return;
    }
    if (e->type != EV_KEY_DOWN) return;
    uint8_t mods = e->key.mods;
    uint8_t sc = e->key.scancode;
    /* Ctrl+S: save */
    if ((mods & MOD_CTRL) && sc == KEY_S) {
        ep_save(st);
        return;
    }
    /* Ctrl+F: open search */
    if ((mods & MOD_CTRL) && sc == KEY_F) {
        st->search_open = 1;
        st->search_len = 0;
        st->search_buf[0] = '\0';
        return;
    }
    /* Esc: close search */
    if (sc == KEY_ESC && st->search_open) {
        st->search_open = 0;
        return;
    }
    /* If search is open, all keys go to the search buffer. */
    if (st->search_open) {
        if (sc == KEY_ENTER) {
            ep_search_next(st);
            return;
        }
        if (keyboard_has_key()) {
            char c = keyboard_getchar();
            if (c == '\b') {
                if (st->search_len > 0) {
                    st->search_len--;
                    st->search_buf[st->search_len] = '\0';
                }
            } else if (c >= 0x20 && c < 0x7F &&
                       st->search_len < (int)sizeof(st->search_buf) - 1) {
                st->search_buf[st->search_len++] = c;
                st->search_buf[st->search_len] = '\0';
            }
        }
        return;
    }
    /* Regular text input. */
    if (keyboard_has_key()) {
        char c = keyboard_getchar();
        if (c == '\n') {
            ep_new_line(st);
            if (st->cursor_row - st->scroll_row >= EP_ROWS) {
                st->scroll_row = st->cursor_row - EP_ROWS + 1;
            }
        } else if (c == '\b') {
            ep_backspace(st);
        } else if (c >= 0x20 && c < 0x7F) {
            ep_insert_char(st, c);
        }
    }
}

/* ---------- public ---------- */
struct widget* editor_pro_create(int x, int y) {
    memset(&ep_state, 0, sizeof(ep_state));
    ep_state.n_lines = 1;
    strcpy(ep_state.filename, "/untitled.c");
    ep_state.active = 1;
    ep_state.dirty = 0;
    ep_state.search_open = 0;

    /* Pre-load a tiny C skeleton so the highlighter has something
     * pretty to show on first open. */
    static const char* skeleton[] = {
        "/* Lestra OS - editor pro */",
        "#include <lestra/types.h>",
        "",
        "int main(void) {",
        "    return 0;",
        "}",
        NULL
    };
    for (int i = 0; skeleton[i]; i++) {
        size_t l = strlen(skeleton[i]);
        if (l >= EP_MAX_LINE_LEN) l = EP_MAX_LINE_LEN - 1;
        memcpy(ep_state.lines[i].text, skeleton[i], l);
        ep_state.lines[i].len = (int)l;
        ep_state.n_lines = i + 2;
    }
    ep_state.n_lines--;

    ep_widget.x = x;
    ep_widget.y = y;
    ep_widget.w = EP_W;
    ep_widget.h = EP_H;
    ep_widget.visible = 1;
    ep_widget.focused = 1;
    ep_widget.draggable = 1;
    ep_widget.resizable = 0;
    ep_widget.draw = ep_draw;
    ep_widget.on_event = ep_on_event;
    ep_widget.state = &ep_state;
    memcpy(ep_widget.title, "Editor Pro", 11);
    return &ep_widget;
}
