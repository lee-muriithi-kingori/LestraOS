/*
 * Lestra OS - Serial Port Driver (16550 UART)
 * Copyright (c) 2026 lestramk.org
 */

#include <lestra/types.h>
#include <lestra/serial.h>

static uint16_t default_port = COM1;

void serial_init(uint16_t port) {
    /* Disable interrupts */
    outb(port + 1, 0x00);
    
    /* Enable DLAB */
    outb(port + 3, 0x80);
    
    /* Set baud rate divisor (low/high) - 115200 / 3 = 38400 */
    outb(port + 0, 0x03);
    outb(port + 1, 0x00);
    
    /* 8 bits, no parity, one stop bit */
    outb(port + 3, 0x03);
    
    /* Enable FIFO, clear them, 14-byte threshold */
    outb(port + 2, 0xC7);
    
    /* IRQs enabled, RTS/DSR set */
    outb(port + 4, 0x0B);
    
    /* Set default */
    default_port = port;
}

bool serial_is_ready(uint16_t port) {
    return (inb(port + 5) & 0x20) != 0;
}

void serial_putchar(uint16_t port, char c) {
    while (!serial_is_ready(port));
    outb(port, c);
}

void serial_puts(uint16_t port, const char* str) {
    while (*str) {
        serial_putchar(port, *str++);
    }
}

char serial_getchar(uint16_t port) {
    while ((inb(port + 5) & 0x01) == 0);
    return inb(port);
}

bool serial_has_data(uint16_t port) {
    return (inb(port + 5) & 0x01) != 0;
}

void serial_default_init(void) {
    serial_init(COM1);
}

void serial_default_putchar(char c) {
    serial_putchar(default_port, c);
}

void serial_default_puts(const char* str) {
    serial_puts(default_port, str);
}
