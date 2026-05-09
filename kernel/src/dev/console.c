#include <dev/console.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <lib/lyrterm.h>
#include <lib/string.h>
#include <sys/poll.h>
#include <util/kprintf.h>
#include <dev/uart.h>

#define LYR_NCCS 19
#define LYR_ECHO 0000010u
#define LYR_ICANON 0000002u
#define LYR_ISIG 0000001u
#define LYR_IEXTEN 0100000u
#define LYR_VTIME 5
#define LYR_VMIN 6
#define CONSOLE_INPUT_SIZE 1024

typedef struct lyr_termios {
	uint32_t c_iflag;
	uint32_t c_oflag;
	uint32_t c_cflag;
	uint32_t c_lflag;
	uint8_t c_line;
	uint8_t c_cc[LYR_NCCS];
	uint32_t c_ispeed;
	uint32_t c_ospeed;
} lyr_termios_t;

static struct {
	volatile uint16_t head;
	volatile uint16_t tail;
	volatile uint16_t count;
	volatile uint16_t lines;
	uint8_t buf[CONSOLE_INPUT_SIZE];
} console_in;

static int console_write(void *ctx, uint64_t off, const void *buf, size_t len,
						 size_t *done);

static lyr_termios_t console_termios = {
	.c_lflag = LYR_ISIG | LYR_ICANON | LYR_ECHO | LYR_IEXTEN,
	.c_ispeed = 38400,
	.c_ospeed = 38400,
};

static void console_wait_input(void)
{
	for (;;) {
		if (console_termios.c_lflag & LYR_ICANON) {
			if (console_in.lines > 0)
				return;
		} else if (console_in.count > 0) {
			return;
		}
		__asm__ volatile("sti; hlt; cli" ::: "memory");
	}
}

static void console_input_push(uint8_t ch)
{
	if (console_in.count >= CONSOLE_INPUT_SIZE)
		return;

	console_in.buf[console_in.head++] = ch;
	console_in.head %= CONSOLE_INPUT_SIZE;
	console_in.count++;

	if (ch == '\n' || ch == '\r')
		console_in.lines++;
}

static int console_input_pop(uint8_t *out)
{
	if (console_in.count == 0)
		return 0;

	*out = console_in.buf[console_in.tail++];
	console_in.tail %= CONSOLE_INPUT_SIZE;
	console_in.count--;

	if ((*out == '\n' || *out == '\r') && console_in.lines > 0)
		console_in.lines--;

	return 1;
}

static int utf8_is_continuation(uint8_t ch)
{
	return (ch & 0xc0u) == 0x80u;
}

static void console_input_backspace(void)
{
	if (console_in.count == 0 || console_in.tail == console_in.head)
		return;

	uint16_t prev = console_in.head ? console_in.head - 1 : CONSOLE_INPUT_SIZE - 1;
	uint8_t old = console_in.buf[prev];
	if (old == '\n' || old == '\r')
		return;

	/* Remove the last byte, then remove UTF-8 continuation bytes until the
	 * leading byte of the same scalar has also been removed. This keeps
	 * backspace from leaving invalid partial UTF-8 in canonical input.
	 */
	for (;;) {
		console_in.head = prev;
		console_in.count--;

		if (!utf8_is_continuation(old) || console_in.count == 0 ||
			console_in.tail == console_in.head)
			break;

		prev = console_in.head ? console_in.head - 1 : CONSOLE_INPUT_SIZE - 1;
		old = console_in.buf[prev];
		if (old == '\n' || old == '\r')
			break;
	}

	if (console_termios.c_lflag & LYR_ECHO)
		console_write(NULL, 0, "\b \b", 3, NULL);
}

void console_input_put(uint8_t ch)
{
	if (ch == '\r')
		ch = '\n';

	if (ch == 8 || ch == 127) {
		console_input_backspace();
		return;
	}

	console_input_push(ch);

	if (console_termios.c_lflag & LYR_ECHO) {
		if (ch == '\n')
			console_write(NULL, 0, "\r\n", 2, NULL);
		else
			console_write(NULL, 0, &ch, 1, NULL);
	}
}

static int console_read(void *ctx, uint64_t off, void *buf, size_t len,
						size_t *done)
{
	(void)ctx;
	(void)off;

	if (done)
		*done = 0;

	if (!buf)
		return VFS_ERR_INVAL;
	if (len == 0)
		return VFS_OK;

	console_wait_input();

	uint8_t *p = buf;
	size_t count = 0;
	int canonical = (console_termios.c_lflag & LYR_ICANON) != 0;

	while (count < len) {
		uint8_t ch;
		if (!console_input_pop(&ch))
			break;
		p[count++] = ch;
		if (canonical && (ch == '\n' || ch == '\r'))
			break;
	}

	if (done)
		*done = count;

	return VFS_OK;
}

static int console_write(void *ctx, uint64_t off, const void *buf, size_t len,
						 size_t *done)
{
	(void)ctx;
	(void)off;

	if (!buf)
		return VFS_ERR_INVAL;

	uart_wbuf(buf, len);
	uart_drain(0);
	lyrterm_wbuf(buf, len);
	lyrterm_flush();

	if (done)
		*done = len;

	return VFS_OK;
}

static int console_poll(void *ctx, int events)
{
	(void)ctx;

	int revents = 0;
	if (events & LYR_POLL_READ_MASK) {
		if ((console_termios.c_lflag & LYR_ICANON) ? console_in.lines > 0 :
											  console_in.count > 0)
			revents |= LYR_POLLIN | LYR_POLLRDNORM;
	}
	if (events & LYR_POLL_WRITE_MASK)
		revents |= LYR_POLLOUT | LYR_POLLWRNORM;
	return revents;
}

static int console_get_winsize(lyr_winsize_t *ws)
{
	if (!ws)
		return VFS_ERR_INVAL;

	uint32_t cols = 0;
	uint32_t rows = 0;
	uint32_t width = 0;
	uint32_t height = 0;

	lyrterm_get_size(&cols, &rows, &width, &height);

	ws->ws_row = (uint16_t)rows;
	ws->ws_col = (uint16_t)cols;
	ws->ws_xpixel = (uint16_t)width;
	ws->ws_ypixel = (uint16_t)height;

	return VFS_OK;
}

static int console_ioctl(void *ctx, unsigned long request, void *arg)
{
	(void)ctx;

	switch (request) {
	case LYR_TCGETS:
		if (!arg)
			return VFS_ERR_INVAL;
		memcpy(arg, &console_termios, sizeof(console_termios));
		return VFS_OK;

	case LYR_TCSETS:
	case LYR_TCSETSW:
	case LYR_TCSETSF:
		if (!arg)
			return VFS_ERR_INVAL;
		memcpy(&console_termios, arg, sizeof(console_termios));
		return VFS_OK;

	case LYR_TIOCGWINSZ:
		return console_get_winsize((lyr_winsize_t *)arg);

	case LYR_TIOCSWINSZ:
		return arg ? VFS_OK : VFS_ERR_INVAL;

	default:
		return VFS_ERR_NOTTY;
	}
}

int console_init(void)
{
	console_termios.c_cc[LYR_VMIN] = 1;
	console_termios.c_cc[LYR_VTIME] = 0;

	int r = devfs_register_chr_poll("/dev/console", 0666, console_read,
								  console_write, console_ioctl, console_poll, NULL);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;

	r = devfs_register_chr_poll("/dev/tty", 0666, console_read, console_write,
								 console_ioctl, console_poll, NULL);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;

	r = devfs_register_chr_poll("/dev/stdin", 0444, console_read, NULL,
								 console_ioctl, console_poll, NULL);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;

	r = devfs_register_chr_poll("/dev/stdout", 0222, NULL, console_write,
								 console_ioctl, console_poll, NULL);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;

	r = devfs_register_chr_poll("/dev/stderr", 0222, NULL, console_write,
								 console_ioctl, console_poll, NULL);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;

	return VFS_OK;
}
