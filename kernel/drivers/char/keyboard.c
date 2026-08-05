/*
 * Lestra OS - PS/2 Keyboard Driver
 * Copyright (c) 2026 lestramk.org
 */

#include <lestra/types.h>
#include <lestra/keyboard.h>
#include <lestra/irq.h>
#include <lestra/idt.h>
#include <lestra/printk.h>
#include <lestra/entropy.h>

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

/* Extended scancode prefix (0xE0) handling.
 * Keys like arrows, Home/End, keypad Enter, and Right Ctrl/Alt
 * send a two-byte sequence starting with 0xE0 in scancode set 1. */
static bool e0_prefix = false;

/* Extended scancode to ASCII lookup for 0xE0-prefixed keys.
 * Only keys that produce ASCII characters are mapped:
 *   0x1C -> '\n' (keypad Enter)
 *   0x35 -> '/'  (keypad /)
 * All other extended scancodes (arrows, F-keys, etc.) are 0. */
static const char e0_scancode_to_ascii[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, '/',0, 0, 0, 0, 0, 0, 0,
};
/* Index 0x1C (28) for KP Enter needs special handling since the array
 * doesn't extend that far — we handle it inline in the IRQ handler. */

static void keyboard_irq_handler(struct interrupt_frame* frame) {
    (void)frame;

    /* KE-16: Feed keyboard event timing into entropy pool */
    uint8_t scancode = inb(KB_DATA_PORT);
    uint64_t now = rdtsc();
    entropy_mix_irq(1, now);

    /* Notify the input subsystem (GUI hook) of ALL scancodes, including
     * modifier keys and release events. This must happen BEFORE the
     * early returns below so the GUI gets complete key state. */
    if (key_handler) {
        key_handler(scancode, 0);
    }

    /* Handle 0xE0 extended prefix byte.
     * Most extended keys arrive as two bytes: 0xE0 then <extended scancode>.
     * The second byte's release has bit 7 set, same as regular scancodes. */
    if (scancode == 0xE0) {
        e0_prefix = true;
        return;
    }

    if (e0_prefix) {
        /* Extended scancode — second byte after 0xE0 prefix. */
        uint8_t ext = scancode & 0x7F;  /* strip release bit for matching */
        e0_prefix = false;

        /* Right Ctrl (0xE0 0x1D / 0x9D) */
        if (ext == KEY_LCTRL) {
            ctrl_pressed = !(scancode & 0x80);
            return;
        }
        /* Right Alt / AltGr (0xE0 0x38 / 0xB8) */
        if (ext == KEY_LALT) {
            alt_pressed = !(scancode & 0x80);
            return;
        }
        /* Keypad Enter (0xE0 0x1C / 0x9C) */
        if (ext == 0x1C && !(scancode & 0x80)) {
            uint8_t next = (key_buffer_head + 1) % KEY_BUFFER_SIZE;
            if (next != key_buffer_tail) {
                key_buffer[key_buffer_head] = '\n';
                key_buffer_head = next;
            }
            return;
        }
        /* Ignore releases of other extended keys */
        if (scancode & 0x80) return;
        /* Try extended ASCII lookup — only '/' (keypad, 0x35) has ASCII */
        if (ext < sizeof(e0_scancode_to_ascii) && e0_scancode_to_ascii[ext]) {
            uint8_t next = (key_buffer_head + 1) % KEY_BUFFER_SIZE;
            if (next != key_buffer_tail) {
                key_buffer[key_buffer_head] = e0_scancode_to_ascii[ext];
                key_buffer_head = next;
            }
        }
        return;
    }

    /* Handle special keys */
    if (scancode == KEY_LSHIFT) {
        shift_pressed = true;
        return;
    }
    if (scancode == (KEY_LSHIFT | 0x80)) {
        shift_pressed = false;
        return;
    }
    if (scancode == KEY_RSHIFT) {
        shift_pressed = true;
        return;
    }
    if (scancode == (KEY_RSHIFT | 0x80)) {
        shift_pressed = false;
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

    /* Ctrl+C → SIGINT */
    if (ctrl_pressed && scancode == 0x2E) {
        extern void signal_send_to_process(void*, int);
        extern void* task_current(void);
        void* cur = task_current();
        if (cur) signal_send_to_process(cur, 2);  /* SIGINT = 2 */
        return;
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
    e0_prefix = false;
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
    /* If the PS/2 keyboard buffer has a key, return true. */
    if (key_buffer_head != key_buffer_tail) return true;
    /* Otherwise, check if there's serial input available on COM1
     * (line status register bit 0 = data ready). This lets the shell
     * accept input from a serial terminal / QEMU's -nographic stdin. */
    extern int serial_has_data(uint16_t port);
    return serial_has_data(0x3F8);
}

uint8_t keyboard_get_scancode(void) {
    return inb(KB_DATA_PORT);
}

char keyboard_getchar(void) {
    while (1) {
        /* Prefer PS/2 keyboard input if available */
        if (key_buffer_head != key_buffer_tail) {
            char c = key_buffer[key_buffer_tail];
            key_buffer_tail = (key_buffer_tail + 1) % KEY_BUFFER_SIZE;
            return c;
        }
        /* Fall back to serial input (COM1) - this is how QEMU's -nographic
         * mode delivers stdin to the guest. Handle \r as \n for line editing. */
        extern int serial_has_data(uint16_t port);
        extern char serial_getchar(uint16_t port);
        if (serial_has_data(0x3F8)) {
            char c = serial_getchar(0x3F8);
            if (c == '\r') c = '\n';
            return c;
        }
        hlt();
    }
}

char keyboard_scancode_to_ascii(uint8_t scancode, bool shift) {
    if (scancode >= sizeof(scancode_to_ascii)) return 0;
    return shift ? scancode_to_ascii_shift[scancode] : scancode_to_ascii[scancode];
}

void keyboard_set_handler(void (*handler)(uint8_t scancode, char ascii)) {
    key_handler = handler;
}

void keyboard_inject_char(char c) {
    /* Push a single ASCII char into the keyboard ring buffer so that
     * keyboard_getchar() / keyboard_has_key() see it, exactly as if
     * the char had arrived from the PS/2 port. Used by the on-screen
     * keyboard (osk.c) and clipboard paste (clipboard.c).
     *
     * The PS/2 IRQ1 handler also writes key_buffer_head / _tail, so we
     * must disable interrupts around the index update to avoid losing
     * a real keypress (or corrupting head/tail). We save RFLAGS first
     * and only re-enable IF if it was set on entry — this way the
     * function is safe even if a caller already holds interrupts off. */
    uint64_t flags = read_flags();
    cli();
    uint8_t next = (key_buffer_head + 1) % KEY_BUFFER_SIZE;
    if (next != key_buffer_tail) {
        key_buffer[key_buffer_head] = c;
        key_buffer_head = next;
    }
    if (flags & 0x200) sti();   /* restore RFLAGS.IF (bit 9) */
}
