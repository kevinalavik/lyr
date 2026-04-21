#ifndef _LYR_UTIL_KPRINTF_H
#define _LYR_UTIL_KPRINTF_H

#include <stddef.h>
#include <stdarg.h>

/* lyr in-kernel kprintf API */
int kprintf(const char *format, ...);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);

#endif // _LYR_UTIL_KPRINTF_H