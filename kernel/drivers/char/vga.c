/*
 * Lestra OS - VGA Text Mode Driver
 * Copyright (c) 2026 lestramk.org
 *
 * Simple 80x25 text mode display with color support.
 */

#include <lestra/types.h>
#include <lestra/vga.h>

/* VGA memory buffer */
static volatile uint16_t* vga_buffer = (volatile uint16_t*)0xFFFFFFFF800B8000;
static uint8_t vga_row = 0;
static uint8_t vga_column = 0;
static uint8_t vga_color;

void vga_init(void) {
    vga_color = vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_row = 0;
    vga_column = 0;
}

void vga_clear(void) {
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_buffer[y * VGA_WIDTH + x] = vga_entry(' ', vga_color);
        }
    }
    vga_row = 0;
    vga_column = 0;
    vga_set_cursor(0, 0);
}

static void update_cursor(void) {
    uint16_t pos = vga_row * VGA_WIDTH + vga_column;
    outb(0x3D4, 14);
    outb(0x3D5, (pos >> 8) & 0xFF);
    outb(0x3D4, 15);
    outb(0x3D5, pos & 0xFF);
}

void vga_putchar(char c) {
    if (c == '\n') {
        vga_column = 0;
        vga_row++;
        if (vga_row >= VGA_HEIGHT) {
            vga_scroll();
        }
    } else if (c == '\r') {
        vga_column = 0;
    } else if (c == '\t') {
        vga_column = (vga_column + 8) & ~7;
        if (vga_column >= VGA_WIDTH) {
            vga_column = 0;
            vga_row++;
        }
    } else if (c == '\b') {
        vga_backspace();
        return;
    } else if (c >= ' ') {
        vga_buffer[vga_row * VGA_WIDTH + vga_column] = vga_entry(c, vga_color);
        vga_column++;
        if (vga_column >= VGA_WIDTH) {
            vga_column = 0;
            vga_row++;
        }
    }
    
    if (vga_row >= VGA_HEIGHT) {
        vga_scroll();
    }
    
    update_cursor();
}

void vga_puts(const char* str) {
    while (*str) {
        vga_putchar(*str++);
    }
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    vga_color = vga_entry_color((enum vga_color)fg, (enum vga_color)bg);
}

void vga_set_cursor(uint8_t x, uint8_t y) {
    vga_column = x;
    vga_row = y;
    update_cursor();
}

void vga_get_cursor(uint8_t* x, uint8_t* y) {
    if (x) *x = vga_column;
    if (y) *y = vga_row;
}

void vga_scroll(void) {
    /* Move all lines up */
    for (int y = 0; y < VGA_HEIGHT - 1; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_buffer[y * VGA_WIDTH + x] = vga_buffer[(y + 1) * VGA_WIDTH + x];
        }
    }
    
    /* Clear last line */
    for (int x = 0; x < VGA_WIDTH; x++) {
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', vga_color);
    }
    
    vga_row = VGA_HEIGHT - 1;
    vga_column = 0;
}

void vga_backspace(void) {
    if (vga_column > 0) {
        vga_column--;
    } else if (vga_row > 0) {
        vga_row--;
        vga_column = VGA_WIDTH - 1;
    }
    vga_buffer[vga_row * VGA_WIDTH + vga_column] = vga_entry(' ', vga_color);
    update_cursor();
}
