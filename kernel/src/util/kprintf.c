#include <util/kprintf.h>
#include <dev/time.h>
#include <dev/uart.h>
#include <lib/string.h>
#include <lib/lyrterm.h>

#define NANOPRINTF_IMPLEMENTATION
#define NANOPRINTF_USE_FIELD_WIDTH_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_PRECISION_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_FLOAT_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_LARGE_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_BINARY_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_WRITEBACK_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_SMALL_FORMAT_SPECIFIERS 1
#include <lib/nanoprintf.h>

static size_t klog_build_prefix(char *buf, size_t size, const char *color,
								const char *subsys)
{
	uint64_t ns = time_monotonic_ns();
	uint64_t sec = ns / (uint64_t)NSEC_PER_SEC;
	uint64_t msec = (ns % (uint64_t)NSEC_PER_SEC) / (uint64_t)NSEC_PER_MSEC;

	int n = npf_snprintf(buf, size, "%s[%02lu.%03lu] %s: ", color ? color : "",
						 (unsigned long)sec, (unsigned long)msec,
						 subsys ? subsys : "kernel");
	if (n < 0)
		return 0;
	if ((size_t)n >= size)
		return size ? size - 1 : 0;
	return (size_t)n;
}

int kprintf(const char *format, ...)
{
	char buf[1024];

	va_list args;
	va_start(args, format);
	int n = npf_vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	size_t len;
	if (n < 0)
		len = 0;
	else if ((size_t)n >= sizeof(buf))
		len = sizeof(buf) - 1;
	else
		len = (size_t)n;

	uart_wbuf(buf, len);
	lyrterm_wbuf(buf, len);
	return (int)len;
}

int klog_vprintf(const char *color, const char *subsys, const char *format,
				 va_list ap)
{
	char line[1408];
	static const char color_reset[] = "\e[0m";
	size_t prefix_len = klog_build_prefix(line, sizeof(line), color, subsys);
	size_t left = prefix_len < sizeof(line) ? sizeof(line) - prefix_len : 0;
	if (left == 0)
		return 0;

	int body = npf_vsnprintf(line + prefix_len, left, format, ap);
	size_t body_len;
	if (body < 0)
		body_len = 0;
	else if ((size_t)body >= left)
		body_len = left - 1;
	else
		body_len = (size_t)body;

	size_t total = prefix_len + body_len;
	if (total + sizeof(color_reset) + 1 < sizeof(line) && color && color[0]) {
		memcpy(line + total, color_reset, sizeof(color_reset) - 1);
		total += sizeof(color_reset) - 1;
	}
	if (total + 1 < sizeof(line))
		line[total++] = '\n';
	line[total] = '\0';

	uart_wbuf(line, total);
	lyrterm_wbuf(line, total);
	return (int)total;
}

int klog_printf(const char *color, const char *subsys, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int n = klog_vprintf(color, subsys, format, ap);
	va_end(ap);
	return n;
}

void kprintf_flush_lyrterm(void)
{
	uart_drain(256);
	lyrterm_flush();
}

size_t kprintf_dropped_lyrterm_bytes(void)
{
	return lyrterm_dropped_bytes();
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
	return npf_vsnprintf(str, size, format, ap);
}
