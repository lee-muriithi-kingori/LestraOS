/*
 * Lestra OS - UI module (splash, menu, panels, themes)
 * Copyright (c) 2026 lestramk.org
 *
 * Enhanced VGA text-mode UI with:
 *   - 3 color themes (cyan-neon, amber-phosphor, green-phosphor)
 *   - Boxed panels with titles
 *   - Animated boot splash with ASCII art
 *   - Menu loop with arrow-style selectors
 *   - Status bar / window chrome
 *
 * All routines write to VGA text-mode memory at 0xB8000 (identity-mapped).
 * Screen is 80x25. Each cell is 2 bytes: [char][attr].
 */

#include <lestra/types.h>
#include <lestra/vga.h>
#include <lestra/printk.h>
#include <lestra/keyboard.h>
#include <lestra/timer.h>
#include <lestra/ui.h>

#define COLS 80
#define ROWS 25

/* ----- theme system --------------------------------------------------- */
/* Theme palettes (fg, bg) for various UI elements */
struct ui_theme {
    const char* name;
    uint8_t title_fg;    /* title bar text */
    uint8_t title_bg;    /* title bar bg */
    uint8_t border;      /* box borders */
    uint8_t body;        /* body text */
    uint8_t accent;      /* highlights */
    uint8_t prompt;      /* shell prompt */
    uint8_t error;       /* errors */
    uint8_t success;     /* success messages */
};

static struct ui_theme themes[] = {
    /* 0: default - cyan neon cyberpunk */
    {
        "cyan-neon",
        .title_fg = 0x0F /* white */,
        .title_bg = 0x03 /* cyan bg */,
        .border   = 0x0B /* light cyan */,
        .body     = 0x0A /* light green */,
        .accent   = 0x0D /* light magenta */,
        .prompt   = 0x0B /* light cyan */,
        .error    = 0x0C /* light red */,
        .success  = 0x0A /* light green */,
    },
    /* 1: amber phosphor (retro CRT) */
    {
        "amber-phosphor",
        .title_fg = 0x00,
        .title_bg = 0x0E /* yellow bg */,
        .border   = 0x0E /* yellow */,
        .body     = 0x06 /* brown/amber */,
        .accent   = 0x0E,
        .prompt   = 0x0E,
        .error    = 0x0C,
        .success  = 0x0A,
    },
    /* 2: green phosphor (matrix-style) */
    {
        "green-phosphor",
        .title_fg = 0x00,
        .title_bg = 0x02 /* green bg */,
        .border   = 0x0A /* light green */,
        .body     = 0x02 /* green */,
        .accent   = 0x0A,
        .prompt   = 0x0A,
        .error    = 0x0C,
        .success  = 0x0A,
    },
};

static int current_theme = 0;  /* default: cyan-neon */

/* ----- cursor --------------------------------------------------------- */
static uint8_t ui_col = 0;
static uint8_t ui_row = 0;

static void put_cell(uint8_t c, uint8_t a, uint8_t row, uint8_t col) {
    /* FIX: was using 0xB8000 + offset (correct, identity-mapped), keep it. */
    volatile uint16_t* vga = (volatile uint16_t*)(uintptr_t)(0xB8000 + (row * COLS + col) * 2);
    *vga = ((uint16_t)a << 8) | c;
}

__attribute__((unused))
static void put_char_at(uint8_t c, uint8_t row, uint8_t col) {
    put_cell(c, 0x0F, row, col);
    ui_col = col + 1;
    if (ui_col >= COLS) {
        ui_col = 0;
        ui_row = row + 1;
    }
}

static void write_str_at(const char* s, uint8_t row, uint8_t col, uint8_t attr) {
    while (*s && col < COLS) {
        put_cell((uint8_t)*s, attr, row, col++);
        s++;
    }
}

static void fill_row(uint8_t row, uint8_t attr) {
    for (int c = 0; c < COLS; c++)
        put_cell(' ', attr, row, (uint8_t)c);
}

/* ----- primitives ----------------------------------------------------- */
void ui_clear(void) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            put_cell(' ', 0x07, (uint8_t)r, (uint8_t)c);
    ui_col = 0; ui_row = 0;
}

void ui_box(uint8_t row, uint8_t col, uint8_t height, uint8_t width, const char* title) {
    uint8_t bdr = themes[current_theme].border;
    uint8_t title_attr = (themes[current_theme].title_bg << 4) | themes[current_theme].title_fg;

    /* Top border with rounded corners */
    put_cell('+', bdr, row, col);
    for (int i = 1; i < width - 1; i++) put_cell('-', bdr, row, col + i);
    put_cell('+', bdr, row, col + width - 1);

    /* Body */
    for (int r = 1; r < height - 1; r++) {
        put_cell('|', bdr, row + r, col);
        for (int i = 1; i < width - 1; i++)
            put_cell(' ', themes[current_theme].body, row + r, col + i);
        put_cell('|', bdr, row + r, col + width - 1);
    }

    /* Bottom border */
    put_cell('+', bdr, row + height - 1, col);
    for (int i = 1; i < width - 1; i++) put_cell('-', bdr, row + height - 1, col + i);
    put_cell('+', bdr, row + height - 1, col + width - 1);

    /* Title centered on top border */
    if (title) {
        int len = 0;
        const char* p = title;
        while (*p++) len++;
        int title_start = col + (width - len - 4) / 2;
        if (title_start < col + 1) title_start = col + 1;
        put_cell('[', title_attr, row, title_start);
        for (int i = 0; i < len && title_start + 1 + i < col + width - 1; i++)
            put_cell((uint8_t)title[i], title_attr, row, title_start + 1 + i);
        put_cell(']', title_attr, row, title_start + len + 1);
    }
}

void ui_panel(uint8_t row, uint8_t col, uint8_t height, uint8_t width,
              const char* title, const char* body) {
    ui_box(row, col, height, width, title);
    /* Body text with word wrap */
    int body_row = row + 1;
    int body_col = col + 2;
    uint8_t body_attr = themes[current_theme].body;
    while (*body && body_row < row + height - 1) {
        if (*body == '\n') {
            body_row++;
            body_col = col + 2;
            body++;
            continue;
        }
        if (body_col >= col + width - 2) {
            body_row++;
            body_col = col + 2;
            if (*body == ' ') body++;
            continue;
        }
        put_cell((uint8_t)*body, body_attr, (uint8_t)body_row, (uint8_t)body_col++);
        body++;
    }
}

/* ----- title bar / status bar ----------------------------------------- */
void ui_titlebar(const char* title) {
    uint8_t attr = (themes[current_theme].title_bg << 4) | themes[current_theme].title_fg;
    fill_row(0, attr);
    write_str_at(title, 0, 2, attr);
    /* Version on the right */
    write_str_at("LestraOS 1.0  x86_64", 0, COLS - 21, attr);
}

void ui_statusbar(const char* msg) {
    uint8_t attr = (themes[current_theme].title_bg << 4) | themes[current_theme].title_fg;
    fill_row(ROWS - 1, attr);
    write_str_at(msg, ROWS - 1, 2, attr);
}

/* ----- splash --------------------------------------------------------- */
void ui_boot_splash(void) {
    ui_clear();
    ui_titlebar(" Lestra OS - by Lee Muriithi Kingori ");

    /* ASCII art logo - cyberpunk style */
    write_str_at("  _                    _           ____   _____ ", 2, 1, themes[current_theme].accent);
    write_str_at(" | |                  | |         / __ \\ / ____|", 3, 1, themes[current_theme].accent);
    write_str_at(" | |    ___  __ _  ___| |_ ___   | |  | | (___  ", 4, 1, themes[current_theme].accent);
    write_str_at(" | |   / _ \\/ _` |/ __| __/ _ \\  | |  | |\\___ \\ ", 5, 1, themes[current_theme].accent);
    write_str_at(" | |__|  __/ (_| | (__| || (_) | | |__| |____) |", 6, 1, themes[current_theme].accent);
    write_str_at(" |_____\\___|\\__,_|\\___|\\__\\___/   \\____/|_____/ ", 7, 1, themes[current_theme].accent);

    /* Author attribution line - prominent */
    write_str_at("              by  Lee Muriithi Kingori              ", 9, 1, themes[current_theme].prompt);
    write_str_at("                lestramk.org  (c) 2026              ", 10, 1, themes[current_theme].body);

    /* Status panel */
    ui_panel(12, 2, 10, 76, "Boot Status",
        "Kernel:     Lestra OS 1.0.0-alpha (x86_64 long mode)\n"
        "Author:     Lee Muriithi Kingori (lestramk.org)\n"
        "Bootloader: GRUB/multiboot2 -> boot.asm (32->64 transition)\n"
        "Memory:     PMM online  VMM online  Heap ready\n"
        "Drivers:    VGA  PS/2 kbd  Serial  PIT  E1000 NIC\n"
        "Network:    DHCP auto-config  HTTP client ready\n"
        "VFS:        in-memory + initrd (max 64 files)\n"
        "AI:         pre-built client (GLM 5.2 / Claude compatible)\n"
        "\nType 'help' at the prompt. Or run the panel menu:\n"
        "  1  System tools   2  Run installer   3  About Lestra OS");
}

/* ----- main menu ------------------------------------------------------ */
static void menu_item(const char* label, uint8_t row, uint8_t attr) {
    write_str_at(" > ", row, 2, attr);
    write_str_at(label, row, 5, attr);
}

void ui_menu_loop(void) {
    ui_clear();
    ui_titlebar(" Lestra OS - Main Menu ");
    uint8_t bdr = themes[current_theme].body;
    uint8_t acc = themes[current_theme].accent;

    write_str_at("Use number keys to select an option:", 2, 2, acc);
    menu_item("1. System tools   (memory, cpu, processes, packages)", 4, bdr);
    menu_item("2. Run installer (writes Lestra image to a target)", 6, bdr);
    menu_item("3. About Lestra OS",                              8, bdr);
    menu_item("4. Switch color theme",                            10, bdr);
    menu_item("5. AI Assistant (chat with API key)",              12, bdr);
    menu_item("0. Drop to shell",                                  14, bdr);

    write_str_at("> ", 22, 0, themes[current_theme].prompt);
    while (1) {
        char c = keyboard_getchar();
        if (c == '1') { ui_system_tools(); ui_clear(); return; }
        if (c == '2') {
            ui_panel(2, 30, 12, 46, "Run Installer",
                "The in-kernel installer is a stub.\n"
                "For real installs use the host-side\n"
                "tools in installer/ (install.py or\n"
                "install.sh). Press any key.");
            keyboard_getchar();
            ui_clear();
            return;
        }
        if (c == '3') {
            ui_panel(2, 22, 12, 54, "About Lestra OS",
                "Lestra OS\n"
                "by Lee Muriithi Kingori\n"
                "lestramk.org  (c) 2026\n"
                "Version 1.0.0-alpha\n"
                "\nA hobbyist x86_64 operating system.\n"
                "Network: E1000 + TCP/IP + HTTP\n"
                "AI: GLM 5.2 / Claude client built in\n"
                "Press any key.");
            keyboard_getchar();
            ui_clear();
            return;
        }
        if (c == '4') {
            current_theme = (current_theme + 1) % 3;
            ui_clear();
            ui_titlebar(" Lestra OS - Main Menu ");
            char buf[80];
            const char* names[] = {"cyan-neon", "amber-phosphor", "green-phosphor"};
            /* build message into buf using simple string ops */
            int i = 0;
            const char* p = "Theme: ";
            while (*p && i < 79) buf[i++] = *p++;
            p = names[current_theme];
            while (*p && i < 79) buf[i++] = *p++;
            buf[i] = 0;
            write_str_at(buf, 2, 2, acc);
            menu_item("1. System tools   (memory, cpu, processes, packages)", 4, bdr);
            menu_item("2. Run installer (writes Lestra image to a target)", 6, bdr);
            menu_item("3. About Lestra OS",                              8, bdr);
            menu_item("4. Switch color theme",                            10, bdr);
            menu_item("5. AI Assistant (chat with API key)",              12, bdr);
            menu_item("0. Drop to shell",                                  14, bdr);
            write_str_at("> ", 22, 0, themes[current_theme].prompt);
            continue;
        }
        if (c == '5') {
            ui_panel(2, 15, 12, 50, "AI Assistant",
                "AI subsystem is available from the\n"
                "shell. Use:\n"
                "  ai keys list\n"
                "  ai keys set <provider> <key>\n"
                "  ai chat <your prompt>\n"
                "  ai tools list\n"
                "\nProviders: openai, claude, gemini, glm\n"
                "Press any key to return.");
            keyboard_getchar();
            ui_clear();
            return;
        }
        if (c == '0') { break; }
    }
    ui_clear();
}

/* ----- system tools panel --------------------------------------------- */
void ui_system_tools(void) {
    ui_clear();
    ui_titlebar(" Lestra OS - System Tools ");

    extern uintptr_t pmm_get_total(void);
    extern uintptr_t pmm_get_used(void);
    extern uintptr_t pmm_get_free(void);
    extern uintptr_t heap_get_used(void);
    extern uint64_t timer_get_ms(void);

    char buf[64];

    /* Memory panel */
    ui_box(2, 2, 9, 36, "Memory");

    write_str_at("Total RAM:", 3, 4, themes[current_theme].body);
    /* simple uitoa */
    {
        unsigned mb = (unsigned)(pmm_get_total() / (1024*1024));
        int i = 0;
        char tmp[16]; int n = 0;
        if (mb == 0) tmp[n++] = '0';
        while (mb) { tmp[n++] = '0' + (mb % 10); mb /= 10; }
        while (n--) buf[i++] = tmp[n];
        buf[i] = 0;
        write_str_at(buf, 3, 16, themes[current_theme].accent);
        write_str_at("MB", 3, 18 + (i > 4 ? i - 4 : 0), themes[current_theme].body);
    }

    write_str_at("Used:      See 'free' command", 5, 4, themes[current_theme].body);
    write_str_at("Free:      See 'free' command", 6, 4, themes[current_theme].body);
    write_str_at("Heap:      See 'meminfo'",       7, 4, themes[current_theme].body);

    /* CPU panel */
    ui_box(2, 40, 9, 38, "CPU");
    write_str_at("Architecture: x86_64",          3, 42, themes[current_theme].body);
    write_str_at("Model:        QEMU Virtual CPU", 4, 42, themes[current_theme].body);
    write_str_at("Cores:        1",                5, 42, themes[current_theme].body);
    write_str_at("Features:     PAE, PSE, Long Mode", 6, 42, themes[current_theme].body);
    write_str_at("Frequency:    ~2000 MHz",        7, 42, themes[current_theme].body);

    /* Uptime panel */
    ui_box(12, 2, 6, 76, "Uptime");
    {
        uint64_t ms = timer_get_ms();
        unsigned sec = (unsigned)(ms / 1000);
        unsigned min = sec / 60;
        unsigned hr = min / 60;
        sec = sec % 60;
        min = min % 60;
        int i = 0;
        char tmp[16]; int n = 0;
        /* hr */
        if (hr == 0) tmp[n++] = '0';
        while (hr) { tmp[n++] = '0' + (hr % 10); hr /= 10; }
        while (n--) buf[i++] = tmp[n];
        buf[i++] = ':';
        n = 0;
        if (min < 10) tmp[n++] = '0';
        while (min) { tmp[n++] = '0' + (min % 10); min /= 10; }
        while (n--) buf[i++] = tmp[n];
        buf[i++] = ':';
        n = 0;
        if (sec < 10) tmp[n++] = '0';
        while (sec) { tmp[n++] = '0' + (sec % 10); sec /= 10; }
        while (n--) buf[i++] = tmp[n];
        buf[i] = 0;
        write_str_at(buf, 14, 30, themes[current_theme].accent);
    }

    /* Process panel */
    ui_box(19, 2, 5, 76, "Processes");
    write_str_at("PID  PPID  STATE    NAME", 20, 4, themes[current_theme].body);
    write_str_at("  0    -1  running  idle", 21, 4, themes[current_theme].body);
    write_str_at("  1     0  running  kernel", 22, 4, themes[current_theme].body);

    write_str_at("Press any key to return...", 24, 2, themes[current_theme].prompt);
    keyboard_getchar();
}

/* ----- theme control -------------------------------------------------- */
int ui_get_theme(void) { return current_theme; }

void ui_set_theme(int t) {
    if (t >= 0 && t < 3) current_theme = t;
}

const char* ui_theme_name(void) {
    return themes[current_theme].name;
}

uint8_t ui_attr_border(void)   { return themes[current_theme].border; }
uint8_t ui_attr_body(void)     { return themes[current_theme].body; }
uint8_t ui_attr_accent(void)   { return themes[current_theme].accent; }
uint8_t ui_attr_prompt(void)   { return themes[current_theme].prompt; }
uint8_t ui_attr_error(void)    { return themes[current_theme].error; }
uint8_t ui_attr_success(void) { return themes[current_theme].success; }
