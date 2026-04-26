#ifndef _LYR_LIB_STRING_H
#define _LYR_LIB_STRING_H

#include <stddef.h>
#include <stdint.h>

void *memset(void *dest, int ch, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

size_t strlen(const char *str);

char *strcpy(char *dest, const char *src);
int strcmp(const char *a, const char *b);

#endif // _LYR_LIB_STRING_H