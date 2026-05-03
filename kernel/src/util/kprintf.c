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

#define CONSOLE_Q_SIZE 8192

static spinlock_t console_lock = SPINLOCK_INIT;

static char console_q[CONSOLE_Q_SIZE];
static size_t console_rpos;
static size_t console_wpos;
static size_t console_dropped;

static bool console_q_empty(void)
{
	return console_rpos == console_wpos;
}

static bool console_q_full(void)
{
	return ((console_wpos + 1) % CONSOLE_Q_SIZE) == console_rpos;
}

static void console_enqueue_locked(const char *s, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (console_q_full()) {
			console_dropped += len - i;
			break;
		}

		console_q[console_wpos] = s[i];
		console_wpos = (console_wpos + 1) % CONSOLE_Q_SIZE;
	}
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

	spinlock_acquire(&console_lock);
	console_enqueue_locked(buf, len);
	spinlock_release(&console_lock);

	uart_wbuf(buf, len);
	return (int)len;
}

void kprintf_flush_lyrterm(void)
{
	char tmp[256];

	for (;;) {
		size_t len = 0;

		if (!spinlock_try_acquire(&console_lock))
			return;

		while (!console_q_empty() && len < sizeof(tmp) - 1) {
			tmp[len++] = console_q[console_rpos];
			console_rpos = (console_rpos + 1) % CONSOLE_Q_SIZE;
		}

		spinlock_release(&console_lock);

		if (len == 0)
			break;

		tmp[len] = '\0';
		lyrterm_putstr(tmp);
	}
}

size_t kprintf_dropped_lyrterm_bytes(void)
{
	size_t dropped = 0;

	if (spinlock_try_acquire(&console_lock)) {
		dropped = console_dropped;
		spinlock_release(&console_lock);
	}

	return dropped;
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
	return npf_vsnprintf(str, size, format, ap);
}