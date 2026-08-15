/* Lestra OS - stddef.h */
#ifndef _STDDEF_H
#define _STDDEF_H

#include <stdint.h>

#ifndef _SIZE_T_DEFINED
typedef uint64_t size_t;
#define _SIZE_T_DEFINED
#endif
typedef int64_t ptrdiff_t;
#ifndef __cplusplus
typedef unsigned int wchar_t;
#endif
#ifndef NULL
#define NULL ((void*)0)
#endif
#define offsetof(type, member) ((size_t)&((type*)0)->member)

#endif

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif
