/*
 * Lestra OS - GUI Terminal card
 * Copyright (c) 2026 lestramk.org / Lee Muriihi Kingori
 *
 * A floating card that embeds the in-kernel shell (lsh). The shell's
 * input/output is redirected to the card's text buffer instead of VGA.
 *
 * This is the simplest possible integration: the terminal card maintains
 * a scrollback buffer of text lines. When the user types, chars are
 * echoed and accumulated into a command line. On Enter, the line is
 * passed to the shell's command dispatcher.
 */

#include <lestra/types.h>
#include <lestra/fb.h>
#include <lestra/input.h>
#include <lestra/gui.h>
#include <lestra/font.h>
#include <lestra/printk.h>
#include <lestra/vga.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <string.h>

/* Terminal card dimensions (from the brief) */
#define TERM_W  720
#define TERM_H  420
#define TERM_TITLE_H 36
#define TERM_PAD 8
#define CHAR_W 8
#define CHAR_H 16
#define TERM_COLS  ((TERM_W - 2 * TERM_PAD) / CHAR_W)
#define TERM_ROWS  ((TERM_H - TERM_TITLE_H - 2 * TERM_PAD) / CHAR_H)

/* Terminal state */
struct term_state {
    char screen[TERM_ROWS][TERM_COLS + 1];
    int cursor_col;
    int cursor_row;
    char input_buf[256];
    int input_len;
    int active;   /* 1 = this terminal has keyboard focus */
};

static struct term_state term_state;
static struct widget term_widget;

/* Forward declarations */
static void term_draw(struct widget* w);
static void term_on_event(struct widget* w, struct event* e);
static void term_scroll(void);
static void term_putc(char c);
static void term_puts(const char* s);
static void term_execute(void);

/* Shell command dispatch (from kernel/core/shell.c) */
extern void shell_execute_line(const char* line, void (*out)(char c));

/* Output redirection: when shell_execute_line calls the output function,
 * we route the chars to our terminal screen. */
static struct term_state* term_output_target = NULL;

static void term_output_char(char c) {
    term_putc(c);
}

static void term_putc(char c) {
    if (c == '\n') {
        term_state.cursor_col = 0;
        term_state.cursor_row++;
        if (term_state.cursor_row >= TERM_ROWS) {
            term_scroll();
            term_state.cursor_row = TERM_ROWS - 1;
        }
        return;
    }
    if (c == '\r') {
        term_state.cursor_col = 0;
        return;
    }
    if (c == '\b') {
        if (term_state.cursor_col > 0) {
            term_state.cursor_col--;
            term_state.screen[term_state.cursor_row][term_state.cursor_col] = ' ';
        }
        return;
    }
    if (c == '\t') {
        for (int i = 0; i < 4; i++) term_putc(' ');
        return;
    }

    if (term_state.cursor_col >= TERM_COLS) {
        term_state.cursor_col = 0;
        term_state.cursor_row++;
        if (term_state.cursor_row >= TERM_ROWS) {
            term_scroll();
            term_state.cursor_row = TERM_ROWS - 1;
        }
    }

    term_state.screen[term_state.cursor_row][term_state.cursor_col] = c;
    term_state.cursor_col++;
}

static void term_puts(const char* s) {
    while (*s) term_putc(*s++);
}

static void term_scroll(void) {
    for (int r = 0; r < TERM_ROWS - 1; r++) {
        memcpy(term_state.screen[r], term_state.screen[r + 1], TERM_COLS);
        term_state.screen[r][TERM_COLS] = '\0';
    }
    memset(term_state.screen[TERM_ROWS - 1], ' ', TERM_COLS);
    term_state.screen[TERM_ROWS - 1][TERM_COLS] = '\0';
}

static void term_clear_screen(void) {
    for (int r = 0; r < TERM_ROWS; r++) {
        memset(term_state.screen[r], ' ', TERM_COLS);
        term_state.screen[r][TERM_COLS] = '\0';
    }
    term_state.cursor_col = 0;
    term_state.cursor_row = 0;
}

static void term_draw_prompt(void) {
    term_puts("lestra:/$ ");
}

static void term_execute(void) {
    /* Null-terminate the input */
    term_state.input_buf[term_state.input_len] = '\0';

    /* Echo newline */
    term_putc('\n');

    /* Execute the command via the shell dispatcher.
     * shell_execute_line takes the line and an output callback. */
    term_output_target = &term_state;
    shell_execute_line(term_state.input_buf, term_output_char);
    term_output_target = NULL;

    /* Reset input buffer */
    term_state.input_len = 0;

    /* Draw new prompt */
    term_draw_prompt();
}

static void term_draw(struct widget* w) {
    struct term_state* ts = (struct term_state*)w->state;

    /* Professional card with close button + title bar */
    extern void ui_draw_card(int x, int y, int w, int h, int focused);
    ui_draw_card(w->x, w->y, w->w, w->h, w->focused);

    /* Title text */
    fb_draw_string(w->x + 70, w->y + 10, "Terminal - lsh 1.0", UI_TEXT_PRIMARY);

    /* Terminal body: black background */
    int body_x = w->x + TERM_PAD;
    int body_y = w->y + TERM_TITLE_H + TERM_PAD;
    int body_w = w->w - 2 * TERM_PAD;
    int body_h = w->h - TERM_TITLE_H - 2 * TERM_PAD;
    fb_fill_rect(body_x, body_y, body_w, body_h, 0xFF000000);

    /* Render text */
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            char ch = ts->screen[r][c];
            if (ch && ch != ' ') {
                fb_draw_char(body_x + c * CHAR_W, body_y + r * CHAR_H,
                             ch, UI_TEXT_PRIMARY);
            }
        }
    }

    /* Cursor (blinking cyan block) */
    uint64_t now = timer_get_ms();
    if ((now / 500) % 2 == 0) {
        int cx = body_x + ts->cursor_col * CHAR_W;
        int cy = body_y + ts->cursor_row * CHAR_H;
        fb_fill_rect(cx, cy, CHAR_W, CHAR_H, UI_ACCENT);
    }
}

static void term_on_event(struct widget* w, struct event* e) {
    struct term_state* ts = (struct term_state*)w->state;

    if (e->type == EV_MOUSE_DOWN) {
        /* Activate this terminal */
        ts->active = 1;
        return;
    }

    if (e->type == EV_KEY_DOWN && ts->active) {
        uint8_t scancode = e->key.scancode;
        uint8_t ascii = e->key.ascii;

        /* The keyboard hook passes ascii=0; we need to decode it ourselves.
         * For printable keys (scancode 0x02-0x36 without 0x80 bit), use
         * the scancode table. But the existing keyboard.c already decoded
         * the ASCII and put it in the key buffer. So we should read from
         * the keyboard buffer instead. */

        /* For the GUI terminal, we read chars from the keyboard buffer
         * (populated by keyboard.c's IRQ handler). This way we get the
         * decoded ASCII including shift state. */
        char c;
        if (keyboard_has_key()) {
            c = keyboard_getchar();
            if (c == '\n') {
                term_execute();
            } else if (c == '\b') {
                if (ts->input_len > 0) {
                    ts->input_len--;
                    term_putc('\b');
                }
            } else if (c >= 0x20 && c < 0x7F) {
                if (ts->input_len < (int)sizeof(ts->input_buf) - 1) {
                    ts->input_buf[ts->input_len++] = c;
                    term_putc(c);
                }
            }
        }
    }
}

/* Public: create and register the terminal widget */
struct widget* terminal_create(int x, int y) {
    term_clear_screen();
    term_state.input_len = 0;
    term_state.active = 1;

    /* Welcome message */
    term_puts("Lestra Shell (lsh) 1.0 - by Lee Muriihi Kingori\n");
    term_puts("Type 'help' for available commands.\n\n");
    term_draw_prompt();

    term_widget.x = x;
    term_widget.y = y;
    term_widget.w = TERM_W;
    term_widget.h = TERM_H;
    term_widget.visible = 1;
    term_widget.focused = 1;
    term_widget.draggable = 1;
    term_widget.resizable = 0;
    term_widget.draw = term_draw;
    term_widget.on_event = term_on_event;
    term_widget.state = &term_state;
    memcpy(term_widget.title, "Terminal", 9);

    return &term_widget;
}
