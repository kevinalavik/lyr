#ifndef _LYR_UTIL_KPRINTF_H
#define _LYR_UTIL_KPRINTF_H

#include <stddef.h>
#include <stdarg.h>

/* lyr in-kernel kprintf API */
int kprintf(const char *format, ...);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int klog_vprintf(const char *color, const char *subsys, const char *format,
				 va_list ap);
int klog_printf(const char *color, const char *subsys, const char *format, ...);

void kprintf_flush_lyrterm(void);
size_t kprintf_dropped_lyrterm_bytes(void);

#endif // _LYR_UTIL_KPRINTF_H
