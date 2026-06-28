/*
 * Lestra OS - PIT Timer Driver
 * Copyright (c) 2026 lestramk.org
 */

#ifndef LESTRA_TIMER_H
#define LESTRA_TIMER_H

#include <lestra/types.h>

#define PIT_FREQUENCY 1193182   /* PIT input frequency in Hz */
#define TIMER_IRQ     0         /* IRQ0 */

void timer_init(uint32_t frequency);
uint64_t timer_get_ticks(void);
uint64_t timer_get_ms(void);
void timer_set_handler(void (*handler)(void));
void timer_wait_ms(uint32_t ms);

/* Scheduler integration */
void timer_schedule_tick(uint32_t hz);

#endif /* LESTRA_TIMER_H */
