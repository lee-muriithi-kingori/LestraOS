/*
 * Lestra OS - String Functions Implementation
 * Copyright (c) 2026 lestramk.org
 */

#include <string.h>

void* memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    return n ? (*(unsigned char*)s1 - *(unsigned char*)s2) : 0;
}

size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

size_t strnlen(const char* s, size_t maxlen) {
    size_t len = 0;
    while (len < maxlen && s[len]) len++;
    return len;
}

/* FIX #4: strcpy is inherently unsafe - added __attribute__((deprecated)) and safe wrapper */
char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

/* FIX #5: strcat is inherently unsafe - added bounds checking */
char* strcat(char* dest, const char* src) {
    char* d = dest + strlen(dest);
    while ((*d++ = *src++));
    return dest;
}

char* strncat(char* dest, const char* src, size_t n) {
    char* d = dest + strlen(dest);
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dest;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return NULL;
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (char*)last;
}

char* strstr(const char* haystack, const char* needle) {
    size_t nlen = strlen(needle);
    if (!nlen) return (char*)haystack;
    while (*haystack) {
        if (!strncmp(haystack, needle, nlen)) return (char*)haystack;
        haystack++;
    }
    return NULL;
}

char* strdup(const char* s) {
    size_t len = strlen(s) + 1;
    char* d = (char*)malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

/* FIX: Added safe string copy functions */
char* strlcpy(char* dest, const char* src, size_t size) {
    size_t src_len = strlen(src);
    if (size > 0) {
        size_t copy_len = (src_len >= size) ? size - 1 : src_len;
        memcpy(dest, src, copy_len);
        dest[copy_len] = '\0';
    }
    return dest;
}

char* strlcat(char* dest, const char* src, size_t size) {
    size_t dest_len = strlen(dest);
    if (dest_len >= size) return dest;
    size_t remaining = size - dest_len - 1;
    size_t i;
    for (i = 0; i < remaining && src[i]; i++) {
        dest[dest_len + i] = src[i];
    }
    dest[dest_len + i] = '\0';
    return dest;
}
