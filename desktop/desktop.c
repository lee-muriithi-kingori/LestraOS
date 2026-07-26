/*
 * Lestra OS - Desktop Environment
 * Copyright (c) 2026 lestramk.org
 *
 * NOT CURRENTLY USED. desktop_init()/desktop_run() are never called —
 * kernel_main.c calls compositor_init()/compositor_run() from
 * kernel/gui/ directly instead. That's the real, working framebuffer
 * GUI (compositor, widgets, top bar, app grid, file explorer, terminal,
 * task manager, etc). This file predates that and was never wired back
 * in. There's also a second, unbuilt duplicate at kernel/desktop/desktop.c
 * (not referenced by the Makefile at all) — pick one and delete the other.
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
