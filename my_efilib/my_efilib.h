// Created by FREE WING,Y.Sakamoto
// http://www.neko.ne.jp/~freewing/

#include <efi.h>
#include <efilib.h>

// #include <limits.h>
#ifndef INT_MAX
// 32bit 0x7FFFFFFF = 2147483647
// 64bit 0x7FFFFFFF FFFFFFFF = 9223372036854775807
#define INT_MAX 2147483647
#endif

// #include <stddef.h>
#ifndef __SIZE_TYPE__
#define __SIZE_TYPE__ long unsigned int
#endif

#ifndef size_t
typedef __SIZE_TYPE__ size_t;
#endif

// #include <stdlib.h>
// Macro
#define abs(a) ((a) < 0 ? (-a) : (a))

// #include <stdlib.h>
// Memory Allocation
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void free(void *ptr);
// #define free(p) FreePool(p)
void *realloc(void *ptr, size_t size);

// #include <string.h>
// memset/memcpy by gnu-efi/lib/init.c
void *memset(void *s, int c, __SIZE_TYPE__ n);
void *memcpy(void *dest, const void *src, __SIZE_TYPE__ n);
// Compare two areas of memory
int memcmp(const void *cs, const void *ct, size_t count);

// #include <types.h>
#ifndef NULL
#define NULL ((void *)0)
#endif

// for upng
#ifndef FILE
#define FILE void *
#define fopen(fn, m) NULL
#define fseek(f, n, s) NULL
#define ftell(f) NULL
#define rewind(f) NULL
#define fclose(f) NULL
#define fread(b, s, n, f) NULL
#endif

