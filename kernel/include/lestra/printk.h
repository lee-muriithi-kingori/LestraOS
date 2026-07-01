/*
 * Lestra OS - Kernel Print Functions
 * Copyright (c) 2026 lestramk.org
 */

#ifndef LESTRA_PRINTK_H
#define LESTRA_PRINTK_H

#include <lestra/types.h>

/* Log levels */
#define KERN_EMERG   "<0>"   /* System is unusable */
#define KERN_ALERT   "<1>"   /* Action must be taken immediately */
#define KERN_CRIT    "<2>"   /* Critical conditions */
#define KERN_ERR     "<3>"   /* Error conditions */
#define KERN_WARN    "<4>"   /* Warning conditions */
#define KERN_NOTICE  "<5>"   /* Normal but significant */
#define KERN_INFO    "<6>"   /* Informational */
#define KERN_DEBUG   "<7>"   /* Debug-level messages */

/* Print functions */
int printk(const char* fmt, ...);
int vprintk(const char* fmt, va_list args);

/* Convenience macros */
#define pr_emerg(fmt, ...)   printk(KERN_EMERG   fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...)   printk(KERN_ALERT   fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...)    printk(KERN_CRIT    fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)     printk(KERN_ERR     fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)    printk(KERN_WARN    fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...)  printk(KERN_NOTICE  fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)    printk(KERN_INFO    fmt, ##__VA_ARGS__)
#ifdef DEBUG
#define pr_debug(fmt, ...)   printk(KERN_DEBUG   fmt, ##__VA_ARGS__)
#else
#define pr_debug(fmt, ...)   do {} while(0)
#endif

/* Early printk (before full initialization) */
void early_printk(const char* str);
void early_putchar(char c);

/* Formatted output helpers */
int print_dec(uint64_t value, char* buf, size_t bufsize);
int print_hex(uint64_t value, char* buf, size_t bufsize, bool uppercase);
int print_bin(uint64_t value, char* buf, size_t bufsize);

#endif /* LESTRA_PRINTK_H */
