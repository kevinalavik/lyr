#include <util/kprintf.h>
#include <dev/uart.h>
#include <lib/lyrterm.h>
#include <sync/spinlock.h>
#include <cpu/instr.h>

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
	static spinlock_t console_lock = SPINLOCK_INIT;

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

	if (interrupts_enabled()) {
		spinlock_acquire(&console_lock);
		uart_wbuf(buf, len);
		lyrterm_putstr(buf);
		spinlock_release(&console_lock);
	} else {
		if (spinlock_try_acquire(&console_lock)) {
			uart_wbuf(buf, len);
			lyrterm_putstr(buf);
			spinlock_release(&console_lock);
		} else {
			uart_wbuf(buf, len);
		}
	}
	return (int)len;
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
	return npf_vsnprintf(str, size, format, ap);
}
