/*
 * Lestra OS - PS/2 Keyboard Driver
 * Copyright (c) 2026 lestramk.org
 */

#include <lestra/types.h>
#include <lestra/keyboard.h>
#include <lestra/irq.h>
#include <lestra/idt.h>
#include <lestra/printk.h>

/* Keyboard ports */
#define KB_DATA_PORT    0x60
#define KB_STATUS_PORT  0x64
#define KB_CMD_PORT     0x64

/* Scancode sets */
#define SC_GET  0x00
#define SC_SET1 0x01
#define SC_SET2 0x02

/* Scancode to ASCII translation (US QWERTY) */
static const char scancode_to_ascii[] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8',
    '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',
    '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0, '*', 0, ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, '-', 0, 0, 0,
    '+', 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0
};

static const char scancode_to_ascii_shift[] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*',
    '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
    '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '\"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0, '*', 0, ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, '-', 0, 0, 0,
    '+', 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0
};

static bool shift_pressed = false;
static bool ctrl_pressed = false;
static bool alt_pressed = false;
static bool caps_lock = false;
static void (*key_handler)(uint8_t scancode, char ascii) = NULL;

/* Simple circular buffer for key events */
#define KEY_BUFFER_SIZE 256
static volatile char key_buffer[KEY_BUFFER_SIZE];
static volatile uint8_t key_buffer_head = 0;
static volatile uint8_t key_buffer_tail = 0;

static void keyboard_irq_handler(struct interrupt_frame* frame) {
    (void)frame;
    
    uint8_t scancode = inb(KB_DATA_PORT);
    
    /* Handle special keys */
    if (scancode == KEY_LSHIFT || scancode == KEY_RSHIFT) {
        shift_pressed = true;
        return;
    }
    if ((scancode & 0x7F) == KEY_LSHIFT || (scancode & 0x7F) == KEY_RSHIFT) {
        if (scancode & 0x80) shift_pressed = false;
        return;
    }
    if (scancode == KEY_LCTRL) {
        ctrl_pressed = true;
        return;
    }
    if (scancode == (KEY_LCTRL | 0x80)) {
        ctrl_pressed = false;
        return;
    }
    if (scancode == KEY_LALT) {
        alt_pressed = true;
        return;
    }
    if (scancode == (KEY_LALT | 0x80)) {
        alt_pressed = false;
        return;
    }
    if (scancode == KEY_CAPSLOCK) {
        caps_lock = !caps_lock;
        return;
    }
    
    /* Ignore key releases */
    if (scancode & 0x80) return;
    
    /* Convert to ASCII */
    bool use_shift = shift_pressed ^ caps_lock;
    char ascii = 0;
    if (scancode < sizeof(scancode_to_ascii)) {
        ascii = use_shift ? scancode_to_ascii_shift[scancode] : scancode_to_ascii[scancode];
    }
    
    /* Call handler if registered */
    if (key_handler) {
        key_handler(scancode, ascii);
    }
    
    /* Store in buffer */
    if (ascii) {
        uint8_t next = (key_buffer_head + 1) % KEY_BUFFER_SIZE;
        if (next != key_buffer_tail) {
            key_buffer[key_buffer_head] = ascii;
            key_buffer_head = next;
        }
    }
}

void keyboard_init(void) {
    shift_pressed = false;
    ctrl_pressed = false;
    alt_pressed = false;
    caps_lock = false;
    key_buffer_head = 0;
    key_buffer_tail = 0;
    
    /* Empty keyboard buffer */
    while (inb(KB_STATUS_PORT) & 1) {
        inb(KB_DATA_PORT);
    }
    
    /* Register IRQ handler */
    register_irq_handler(1, keyboard_irq_handler);
    irq_enable(1);
    
    pr_debug("Keyboard initialized\n");
}

bool keyboard_has_key(void) {
    return key_buffer_head != key_buffer_tail;
}

uint8_t keyboard_get_scancode(void) {
    return inb(KB_DATA_PORT);
}

char keyboard_getchar(void) {
    while (key_buffer_head == key_buffer_tail) {
        hlt();
    }
    
    char c = key_buffer[key_buffer_tail];
    key_buffer_tail = (key_buffer_tail + 1) % KEY_BUFFER_SIZE;
    return c;
}

char keyboard_scancode_to_ascii(uint8_t scancode, bool shift) {
    if (scancode >= sizeof(scancode_to_ascii)) return 0;
    return shift ? scancode_to_ascii_shift[scancode] : scancode_to_ascii[scancode];
}

void keyboard_set_handler(void (*handler)(uint8_t scancode, char ascii)) {
    key_handler = handler;
}
