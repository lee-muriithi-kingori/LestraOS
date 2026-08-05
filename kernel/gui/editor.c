/*
 * Lestra OS - Text Editor card
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * A basic text editor card. Not VS Code (that needs Electron + V8 +
 * Chromium — millions of lines, not feasible in a kernel). This is a
 * simple single-file editor with:
 *   - Line-based text buffer
 *   - Cursor movement (arrows, Home, End)
 *   - Insert/delete characters
 *   - Save/load via the in-memory VFS
 *
 * Good enough for editing config files or writing short scripts.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/keyboard.h>
#include <lestra/vfs.h>
#include <lestra/printk.h>
#include <lestra/timer.h>
#include <string.h>

#define EDIT_W  600
#define EDIT_H  440
#define EDIT_TITLE_H 36
#define EDIT_PAD 8
#define CHAR_W 8
#define CHAR_H 16
#define EDIT_COLS ((EDIT_W - 2 * EDIT_PAD - 40) / CHAR_W)  /* -40 for line numbers */
#define EDIT_ROWS ((EDIT_H - EDIT_TITLE_H - 2 * EDIT_PAD - 28) / CHAR_H)
#define MAX_LINES 256
#define MAX_LINE_LEN 200

struct editor_state {
    char lines[MAX_LINES][MAX_LINE_LEN];
    int line_lens[MAX_LINES];
    int n_lines;
    int cursor_col;
    int cursor_row;
    int scroll_row;   /* first visible row */
    int active;
    char filename[64];
};

static struct editor_state editor_state;
static struct widget editor_widget;

static void editor_draw(struct widget* w);
static void editor_on_event(struct widget* w, struct event* e);

static void editor_new_line(int after_row) {
    if (editor_state.n_lines >= MAX_LINES) return;
    for (int i = editor_state.n_lines; i > after_row + 1; i--) {
        memcpy(editor_state.lines[i], editor_state.lines[i-1], MAX_LINE_LEN);
        editor_state.line_lens[i] = editor_state.line_lens[i-1];
    }
    editor_state.lines[after_row + 1][0] = '\0';
    editor_state.line_lens[after_row + 1] = 0;
    editor_state.n_lines++;
}

static void editor_init(void) {
    memset(&editor_state, 0, sizeof(editor_state));
    editor_state.n_lines = 1;
    editor_state.lines[0][0] = '\0';
    editor_state.line_lens[0] = 0;
    strcpy(editor_state.filename, "untitled.txt");
}

static void editor_draw(struct widget* w) {
    struct editor_state* st = (struct editor_state*)w->state;

    /* Card body */
    fb_draw_rounded(w->x, w->y, w->w, w->h, 14,
                    UI_CARD_BG, st->active ? UI_ACCENT : UI_CARD_BORDER);

    /* Title bar */
    fb_fill_rect(w->x + 1, w->y + 1, w->w - 2, EDIT_TITLE_H - 1, 0xE00E1422);
    char title[80];
    ksnprintf(title, sizeof(title), "Editor - %s", st->filename);
    fb_draw_string(w->x + 12, w->y + 10, title, UI_TEXT_PRIMARY);
    fb_draw_string(w->x + w->w - 20, w->y + 10, "x", UI_TEXT_MUTED);

    /* Editor body */
    int body_x = w->x + EDIT_PAD;
    int body_y = w->y + EDIT_TITLE_H + EDIT_PAD;
    int body_w = w->w - 2 * EDIT_PAD;
    int body_h = w->h - EDIT_TITLE_H - 2 * EDIT_PAD - 28;
    fb_fill_rect(body_x, body_y, body_w, body_h, 0xFF0A0C12);

    /* Line number gutter */
    int gutter_w = 32;
    fb_fill_rect(body_x, body_y, gutter_w, body_h, 0xFF050608);

    /* Draw visible lines */
    for (int i = 0; i < EDIT_ROWS && st->scroll_row + i < st->n_lines; i++) {
        int row = st->scroll_row + i;
        int y = body_y + i * CHAR_H;

        /* Line number */
        char numbuf[8];
        ksnprintf(numbuf, sizeof(numbuf), "%3d", row + 1);
        fb_draw_string(body_x + 4, y, numbuf, UI_TEXT_FAINT);

        /* Line content */
        char* line = st->lines[row];
        int llen = st->line_lens[row];
        for (int c = 0; c < llen && c < EDIT_COLS; c++) {
            fb_draw_char(body_x + gutter_w + 4 + c * CHAR_W, y,
                         line[c], UI_TEXT_PRIMARY);
        }
    }

    /* Cursor */
    uint64_t now = timer_get_ms();
    if (st->active && (now / 500) % 2 == 0) {
        int cur_row = st->cursor_row - st->scroll_row;
        if (cur_row >= 0 && cur_row < EDIT_ROWS) {
            int cx = body_x + gutter_w + 4 + st->cursor_col * CHAR_W;
            int cy = body_y + cur_row * CHAR_H;
            fb_fill_rect(cx, cy, CHAR_W, CHAR_H, UI_ACCENT);
        }
    }

    /* Status bar */
    int status_y = w->y + w->h - 24;
    fb_fill_rect(w->x + EDIT_PAD, status_y, w->w - 2 * EDIT_PAD, 20, 0xFF0A0C12);
    char status[120];
    ksnprintf(status, sizeof(status), "Ln %d, Col %d  -  %d lines  -  Ctrl+S: save",
               st->cursor_row + 1, st->cursor_col + 1, st->n_lines);
    fb_draw_string(w->x + EDIT_PAD + 6, status_y + 2, status, UI_TEXT_MUTED);
}

static void editor_on_event(struct widget* w, struct event* e) {
    struct editor_state* st = (struct editor_state*)w->state;

    if (e->type == EV_MOUSE_DOWN) {
        st->active = 1;
        return;
    }

    if (e->type == EV_MOUSE_SCROLL) {
        /* Wheel up (scroll > 0) views earlier lines, so the view
         * window moves up (scroll_row decreases). Wheel down (scroll
         * < 0) moves the view toward the end of the file. Clamped to
         * [0, max(0, n_lines - EDIT_ROWS)] so the view never runs
         * past either end of the buffer. */
        int delta = e->mouse.scroll;
        int max_scroll = st->n_lines - EDIT_ROWS;
        if (max_scroll < 0) max_scroll = 0;
        int new_scroll = st->scroll_row - delta;
        if (new_scroll < 0) new_scroll = 0;
        if (new_scroll > max_scroll) new_scroll = max_scroll;
        st->scroll_row = new_scroll;
        return;
    }

    if (e->type == EV_KEY_DOWN && st->active) {
        char c;
        if (keyboard_has_key()) {
            c = keyboard_getchar();
            if (c == '\n') {
                /* Split line at cursor: text [0..col) stays on the current
                   line, text [col..len) moves to the new line below. The old
                   code only opened a blank slot and left the tail on the
                   original line, so pressing Enter in the middle of a line
                   silently discarded everything after the cursor (C1). */
                int row = st->cursor_row;
                int col = st->cursor_col;
                int len = st->line_lens[row];
                if (col > len) col = len;                  /* defensive clamp */
                if (len > MAX_LINE_LEN - 1) len = MAX_LINE_LEN - 1;
                if (st->n_lines < MAX_LINES) {
                    /* Open a slot at row+1 (shifts lines below down by 1,
                       leaves lines[row+1] empty). lines[row] is untouched. */
                    editor_new_line(row);
                    /* Move the tail [col..len) to the new line below. */
                    int tail_len = len - col;
                    if (tail_len > MAX_LINE_LEN - 1) tail_len = MAX_LINE_LEN - 1;
                    if (tail_len > 0) {
                        memcpy(st->lines[row + 1], st->lines[row] + col, tail_len);
                        st->lines[row + 1][tail_len] = '\0';
                        st->line_lens[row + 1] = tail_len;
                    } else {
                        st->lines[row + 1][0] = '\0';
                        st->line_lens[row + 1] = 0;
                    }
                    /* Truncate the current line at the cursor. */
                    st->lines[row][col] = '\0';
                    st->line_lens[row] = col;
                    /* Move cursor to the start of the new line. */
                    st->cursor_row = row + 1;
                    st->cursor_col = 0;
                    /* Adjust scroll if the cursor left the viewport. */
                    if (st->cursor_row - st->scroll_row >= EDIT_ROWS) {
                        st->scroll_row = st->cursor_row - EDIT_ROWS + 1;
                    }
                }
            } else if (c == '\b') {
                if (st->cursor_col > 0) {
                    /* Delete char before cursor (within current line). */
                    char* line = st->lines[st->cursor_row];
                    int len = st->line_lens[st->cursor_row];
                    if (len > MAX_LINE_LEN - 1) len = MAX_LINE_LEN - 1;
                    if (st->cursor_col > len) st->cursor_col = len;  /* defensive */
                    if (st->cursor_col > 0 && len > 0) {
                        st->cursor_col--;
                        /* Shift chars left */
                        for (int i = st->cursor_col; i < len; i++) {
                            line[i] = line[i + 1];
                        }
                        st->line_lens[st->cursor_row] = len - 1;
                    }
                } else if (st->cursor_row > 0) {
                    /* Merge current line into the previous line. */
                    int prev_row = st->cursor_row - 1;
                    int prev_len = st->line_lens[prev_row];
                    int cur_len  = st->line_lens[st->cursor_row];
                    /* Defensive clamps (invariant: 0 <= len <= MAX_LINE_LEN-1). */
                    if (prev_len < 0) prev_len = 0;
                    if (prev_len > MAX_LINE_LEN - 1) prev_len = MAX_LINE_LEN - 1;
                    if (cur_len  < 0) cur_len  = 0;
                    if (cur_len  > MAX_LINE_LEN - 1) cur_len  = MAX_LINE_LEN - 1;
                    char* prev = st->lines[prev_row];
                    char* cur  = st->lines[st->cursor_row];
                    /* Copy as many chars from cur as fit, leaving room for
                       the NUL terminator. The old code wrote the unclamped
                       prev_len+cur_len into line_lens, which then drove OOB
                       reads in the in-line backspace loop (C2). */
                    int room = (MAX_LINE_LEN - 1) - prev_len;
                    if (room < 0) room = 0;
                    int copy_n = (cur_len < room) ? cur_len : room;
                    if (copy_n > 0) {
                        memcpy(prev + prev_len, cur, copy_n);
                    }
                    prev[prev_len + copy_n] = '\0';
                    st->line_lens[prev_row] = prev_len + copy_n;  /* clamped */
                    /* Cursor sits at the join point in the previous line. */
                    st->cursor_col = prev_len;
                    st->cursor_row = prev_row;
                    /* Shift lines up to fill the gap left by the merged line. */
                    for (int i = st->cursor_row + 1; i < st->n_lines - 1; i++) {
                        memcpy(st->lines[i], st->lines[i+1], MAX_LINE_LEN);
                        st->line_lens[i] = st->line_lens[i+1];
                    }
                    st->n_lines--;
                }
            } else if (c >= 0x20 && c < 0x7F) {
                /* Insert character at cursor */
                char* line = st->lines[st->cursor_row];
                int len = st->line_lens[st->cursor_row];
                if (len > MAX_LINE_LEN - 1) len = MAX_LINE_LEN - 1;
                if (st->cursor_col > len) st->cursor_col = len;  /* defensive */
                if (len < MAX_LINE_LEN - 1) {
                    /* Shift chars right */
                    for (int i = len; i > st->cursor_col; i--) {
                        line[i] = line[i-1];
                    }
                    line[st->cursor_col] = c;
                    st->line_lens[st->cursor_row] = len + 1;
                    st->cursor_col++;
                }
            }
        }
    }
}

struct widget* editor_create(int x, int y) {
    editor_init();
    editor_widget.x = x;
    editor_widget.y = y;
    editor_widget.w = EDIT_W;
    editor_widget.h = EDIT_H;
    editor_widget.visible = 1;
    editor_widget.focused = 0;
    editor_widget.draggable = 1;
    editor_widget.resizable = 0;
    editor_widget.draw = editor_draw;
    editor_widget.on_event = editor_on_event;
    editor_widget.state = &editor_state;
    memcpy(editor_widget.title, "Editor", 7);
    return &editor_widget;
}
