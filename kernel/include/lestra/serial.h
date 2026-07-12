/*
 * Lestra OS - Serial Port Driver
 * Copyright (c) 2026 lestramk.org
 */

#ifndef LESTRA_SERIAL_H
#define LESTRA_SERIAL_H

#include <lestra/types.h>

/* COM ports */
#define COM1 0x3F8
#define COM2 0x2F8
#define COM3 0x3E8
#define COM4 0x2E8

void serial_init(uint16_t port);
bool serial_is_ready(uint16_t port);
void serial_putchar(uint16_t port, char c);
void serial_puts(uint16_t port, const char* str);
char serial_getchar(uint16_t port);
bool serial_has_data(uint16_t port);

/* Default serial output */
void serial_default_init(void);
void serial_default_putchar(char c);
void serial_default_puts(const char* str);

#endif /* LESTRA_SERIAL_H */
