#include <util/kprintf.h>
#include <dev/uart.h>

#define NANOPRINTF_IMPLEMENTATION
#define NANOPRINTF_USE_FIELD_WIDTH_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_PRECISION_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_FLOAT_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_LARGE_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_BINARY_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_WRITEBACK_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_SMALL_FORMAT_SPECIFIERS 1
#include <lib/nanoprintf.h>

int kprintf(const char *format, ...)
{
	char buf[256];
	va_list args;
	va_start(args, format);
	int len = npf_vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	uart_wbuf(buf, len);
	return len;
}