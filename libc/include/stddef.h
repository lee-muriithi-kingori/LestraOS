/* Lestra OS - stddef.h */
#ifndef _STDDEF_H
#define _STDDEF_H

typedef long ptrdiff_t;
typedef unsigned long size_t;
#ifndef __cplusplus
typedef unsigned int wchar_t;
#endif
#define NULL ((void*)0)
#define offsetof(type, member) ((size_t)&((type*)0)->member)

#endif
