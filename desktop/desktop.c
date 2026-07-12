/*
 * Lestra OS - Desktop Environment
 * Copyright (c) 2026 lestramk.org
 *
 * For now, the "desktop" is the VGA text-mode UI provided by kernel/ui/.
 * When framebuffer graphics are added (planned), this module will own the
 * graphical desktop, windows, and compositor.
 */
#include <lestra/types.h>
#include <lestra/printk.h>
#include <lestra/ui.h>

void desktop_init(void) {
    pr_info("Desktop: text-mode UI ready (framebuffer planned)\n");
}

void desktop_run(void) {
    /* Launch the UI menu loop. The user can drop to shell from there. */
    ui_boot_splash();
    ui_menu_loop();
}
