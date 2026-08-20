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
     * timing entropy on real hardware (+-microsecond jitter from
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
    sched_tick();

    /* Pump the network stack. */
    extern void net_tick(void);
    net_tick();

    /* Cron daemon -- check scheduled tasks every second */
    static uint64_t last_cron_tick = 0;
    if (ticks - last_cron_tick >= frequency) {
        last_cron_tick = ticks;
        extern void cron_tick(void);
        cron_tick();
        extern void service_tick(void);
        service_tick();
    }

    /* MAC address changer -- check every 10 minutes */
    static uint64_t last_mac_tick = 0;
    uint64_t mac_interval = (uint64_t)frequency * 600;  /* 10 minutes in ticks */
    if (ticks - last_mac_tick >= mac_interval) {
        last_mac_tick = ticks;
        extern void mac_changer_tick(void);
        mac_changer_tick();
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
