/*
 * Lestra OS - PIT Timer Driver
 * Copyright (c) 2026 lestramk.org
 *
 * Programmable Interval Timer for scheduling and timing.
 */

#include <lestra/types.h>
#include <lestra/timer.h>
#include <lestra/irq.h>
#include <lestra/idt.h>
#include <lestra/printk.h>

static volatile uint64_t ticks = 0;
static uint32_t frequency = 0;
static void (*tick_handler)(void) = NULL;

static void timer_irq_handler(struct interrupt_frame* frame) {
    (void)frame;
    ticks++;
    
    if (tick_handler) {
        tick_handler();
    }
}

void timer_init(uint32_t freq) {
    frequency = freq;
    ticks = 0;
    
    /* Calculate divisor */
    uint32_t divisor = PIT_FREQUENCY / freq;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;
    
    /* Send command: channel 0, lobyte/hibyte, mode 3 (square wave) */
    outb(0x43, 0x36);
    
    /* Send divisor */
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    
    /* Register IRQ handler */
    register_irq_handler(0, timer_irq_handler);
    irq_enable(0);
    
    pr_debug("Timer initialized at %u Hz (divisor=%u)\n", freq, divisor);
}

uint64_t timer_get_ticks(void) {
    return ticks;
}

uint64_t timer_get_ms(void) {
    return (ticks * 1000) / frequency;
}

void timer_set_handler(void (*handler)(void)) {
    tick_handler = handler;
}

void timer_wait_ms(uint32_t ms) {
    uint64_t target = timer_get_ms() + ms;
    while (timer_get_ms() < target) {
        hlt();
    }
}

void timer_schedule_tick(uint32_t hz) {
    timer_init(hz);
}
