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
#include <lestra/entropy.h>

static volatile uint64_t ticks = 0;
static uint32_t frequency = 0;
static void (*tick_handler)(void) = NULL;

static void timer_irq_handler(struct interrupt_frame* frame) {
    (void)frame;
    ticks++;

    /* KE-16: Feed TSC jitter into interrupt-mixed entropy pool.
     * The TSC delta between timer fires contains real hardware
     * timing entropy on real hardware (±microsecond jitter from
     * interrupt latency). On QEMU the jitter is near-zero but still
     * adds mixing diversity with the keyboard/mouse pool feeds. */
    static uint64_t last_tsc = 0;
    uint64_t now = rdtsc();
    entropy_mix_irq(0, now - last_tsc);
    last_tsc = now;

    if (tick_handler) {
        tick_handler();
    }

    extern void sched_tick(void);
    /* KE-25: sched_tick() already calls schedule() when the scheduler is
     * enabled. The previous UNCONDITIONAL schedule(frame) call here fired
     * a context switch on every timer IRQ even with preemption disabled,
     * which — once PID 1 became `current` — drove the broken
     * context_switch path every tick and triple-faulted. Removing the
     * duplicate call means context switches only happen when the
     * scheduler is explicitly enabled. */
    sched_tick();

    /* Pump the network stack. net_tick() is a no-op if net_init() hasn't
     * been called yet or if there's no NIC. Called from IRQ0 context so
     * it must be cheap; net_tick() drains only a bounded number of
     * packets per call. */
    extern void net_tick(void);
    net_tick();

    /* Cron daemon — check scheduled tasks every second */
    static uint64_t last_cron_tick = 0;
    if (ticks - last_cron_tick >= frequency) {  /* every 1 second */
        last_cron_tick = ticks;
        extern void cron_tick(void);
        cron_tick();
        /* Service manager — check services and SSH sessions */
        extern void service_tick(void);
        service_tick();
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

/* KE-26: Expose the timer frequency for /proc/uptime. */
uint32_t timer_get_frequency(void) {
    return frequency;
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
