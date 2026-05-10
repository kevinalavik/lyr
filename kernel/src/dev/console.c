#include <dev/console.h>
#include <dev/uart.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <lib/lyrterm.h>
#include <lib/string.h>
#include <sys/poll.h>

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

typedef struct console_input_ring {
	volatile uint16_t head;
	volatile uint16_t tail;
	volatile uint16_t count;
	volatile uint16_t lines;
	uint8_t buf[CONSOLE_INPUT_SIZE];
} console_input_ring_t;

typedef struct console_tty {
	unsigned index;
	char name[LYR_TTY_NAME_MAX];
	console_input_ring_t input;
	lyr_termios_t termios;
	lyrterm_state_t render;
} console_tty_t;

static console_tty_t consoles[LYR_TTY_COUNT];
static unsigned active_console;

static int console_write(void *ctx, uint64_t off, const void *buf, size_t len,
						 size_t *done);

static console_tty_t *console_ctx_to_tty(void *ctx)
{
	if (!ctx)
		return &consoles[active_console];
	return (console_tty_t *)ctx;
}

static void console_tty_name(unsigned index, char *buf, size_t cap)
{
	if (!buf || cap < 5)
		return;

	buf[0] = 't';
	buf[1] = 't';
	buf[2] = 'y';
	buf[3] = (char)('1' + index);
	buf[4] = '\0';
}

static void console_tty_init(console_tty_t *tty, unsigned index)
{
	memset(tty, 0, sizeof(*tty));
	tty->index = index;
	tty->termios.c_lflag = LYR_ISIG | LYR_ICANON | LYR_ECHO | LYR_IEXTEN;
	tty->termios.c_ispeed = 38400;
	tty->termios.c_ospeed = 38400;
	tty->termios.c_cc[LYR_VMIN] = 1;
	tty->termios.c_cc[LYR_VTIME] = 0;
	console_tty_name(index, tty->name, sizeof(tty->name));
	lyrterm_capture_state(&tty->render);
}

static void console_wait_input(console_tty_t *tty)
{
	if (tty->index == active_console)
		lyrterm_flush();

	for (;;) {
		if (tty->termios.c_lflag & LYR_ICANON) {
			if (tty->input.lines > 0)
				return;
		} else if (tty->input.count > 0) {
			return;
		}
		__asm__ volatile("sti; hlt; cli" ::: "memory");
	}
}

static void console_input_push(console_tty_t *tty, uint8_t ch)
{
	console_input_ring_t *in = &tty->input;
	if (in->count >= CONSOLE_INPUT_SIZE)
		return;

	in->buf[in->head++] = ch;
	in->head %= CONSOLE_INPUT_SIZE;
	in->count++;

	if (ch == '\n' || ch == '\r')
		in->lines++;
}

static int console_input_pop(console_tty_t *tty, uint8_t *out)
{
	console_input_ring_t *in = &tty->input;
	if (in->count == 0)
		return 0;

	*out = in->buf[in->tail++];
	in->tail %= CONSOLE_INPUT_SIZE;
	in->count--;

	if ((*out == '\n' || *out == '\r') && in->lines > 0)
		in->lines--;

	return 1;
}

static int utf8_is_continuation(uint8_t ch)
{
	return (ch & 0xc0u) == 0x80u;
}

static void console_input_backspace(console_tty_t *tty)
{
	console_input_ring_t *in = &tty->input;
	if (in->count == 0 || in->tail == in->head)
		return;

	uint16_t prev = in->head ? in->head - 1 : CONSOLE_INPUT_SIZE - 1;
	uint8_t old = in->buf[prev];
	if (old == '\n' || old == '\r')
		return;

	for (;;) {
		in->head = prev;
		in->count--;

		if (!utf8_is_continuation(old) || in->count == 0 || in->tail == in->head)
			break;

		prev = in->head ? in->head - 1 : CONSOLE_INPUT_SIZE - 1;
		old = in->buf[prev];
		if (old == '\n' || old == '\r')
			break;
	}

	if ((tty->termios.c_lflag & LYR_ECHO) && tty->index == active_console)
		console_write(tty, 0, "\b \b", 3, NULL);
}

void console_input_put(uint8_t ch)
{
	console_tty_t *tty = &consoles[active_console];
	int canonical = (tty->termios.c_lflag & LYR_ICANON) != 0;
	int echo = (tty->termios.c_lflag & LYR_ECHO) != 0;

	if (ch == '\r')
		ch = '\n';

	if (canonical && (ch == 8 || ch == 127)) {
		console_input_backspace(tty);
		return;
	}

	console_input_push(tty, ch);

	if (!echo)
		return;

	if (ch == '\n')
		console_write(tty, 0, "\r\n", 2, NULL);
	else
		console_write(tty, 0, &ch, 1, NULL);
}

static int console_read(void *ctx, uint64_t off, void *buf, size_t len,
						size_t *done)
{
	(void)off;
	console_tty_t *tty = console_ctx_to_tty(ctx);

	if (done)
		*done = 0;
	if (!buf)
		return -EINVAL;
	if (len == 0)
		return 0;

	console_wait_input(tty);

	uint8_t *p = buf;
	size_t count = 0;
	int canonical = (tty->termios.c_lflag & LYR_ICANON) != 0;

	while (count < len) {
		uint8_t ch;
		if (!console_input_pop(tty, &ch))
			break;
		p[count++] = ch;
		if (canonical && (ch == '\n' || ch == '\r'))
			break;
	}

	if (done)
		*done = count;

	return 0;
}

static int console_write(void *ctx, uint64_t off, const void *buf, size_t len,
						 size_t *done)
{
	(void)off;
	console_tty_t *tty = console_ctx_to_tty(ctx);

	if (!buf)
		return -EINVAL;

	if (tty->index == active_console) {
		uart_wbuf(buf, len);
		lyrterm_wbuf(buf, len);
	} else {
		lyrterm_update_state(&tty->render, buf, len);
	}

	if (done)
		*done = len;

	return 0;
}

static int console_poll(void *ctx, int events)
{
	console_tty_t *tty = console_ctx_to_tty(ctx);
	int revents = 0;

	if (events & LYR_POLL_READ_MASK) {
		if ((tty->termios.c_lflag & LYR_ICANON) ? tty->input.lines > 0 :
													 tty->input.count > 0)
			revents |= LYR_POLLIN | LYR_POLLRDNORM;
	}
	if (events & LYR_POLL_WRITE_MASK)
		revents |= LYR_POLLOUT | LYR_POLLWRNORM;
	return revents;
}

static int console_get_winsize(lyr_winsize_t *ws)
{
	if (!ws)
		return -EINVAL;

	uint32_t cols = 0;
	uint32_t rows = 0;
	uint32_t width = 0;
	uint32_t height = 0;

	lyrterm_get_size(&cols, &rows, &width, &height);

	ws->ws_row = (uint16_t)rows;
	ws->ws_col = (uint16_t)cols;
	ws->ws_xpixel = (uint16_t)width;
	ws->ws_ypixel = (uint16_t)height;
	return 0;
}

static int console_ioctl(void *ctx, unsigned long request, void *arg)
{
	console_tty_t *tty = console_ctx_to_tty(ctx);

	switch (request) {
	case LYR_TCGETS:
		if (!arg)
			return -EINVAL;
		memcpy(arg, &tty->termios, sizeof(tty->termios));
		return 0;

	case LYR_TCSETS:
	case LYR_TCSETSW:
	case LYR_TCSETSF:
		if (!arg)
			return -EINVAL;
		memcpy(&tty->termios, arg, sizeof(tty->termios));
		return 0;

	case LYR_TIOCGWINSZ:
		return console_get_winsize((lyr_winsize_t *)arg);

	case LYR_TIOCSWINSZ:
		return arg ? 0 : -EINVAL;

	case LYR_TIOCGNAME:
		if (!arg)
			return -EINVAL;
		memcpy(arg, tty->name, strlen(tty->name) + 1);
		return 0;

	default:
		return -ENOTTY;
	}
}

int console_switch_tty(unsigned index)
{
	if (index >= LYR_TTY_COUNT || index == active_console)
		return index < LYR_TTY_COUNT ? 0 : -EINVAL;

	lyrterm_capture_state(&consoles[active_console].render);
	active_console = index;
	lyrterm_restore_state(&consoles[active_console].render);
	return 0;
}

unsigned console_active_tty(void)
{
	return active_console;
}

int console_init(void)
{
	for (unsigned i = 0; i < LYR_TTY_COUNT; i++)
		console_tty_init(&consoles[i], i);

	for (unsigned i = 0; i < LYR_TTY_COUNT; i++) {
		char path[32];
		memcpy(path, "/dev/", 5);
		memcpy(path + 5, consoles[i].name, strlen(consoles[i].name) + 1);
		int r = devfs_register_chr_poll(path, 0666, console_read, console_write,
										console_ioctl, console_poll, &consoles[i]);
		if (r != 0 && r != -EEXIST)
			return r;
	}

	int r = devfs_register_chr_poll("/dev/console", 0666, console_read,
									console_write, console_ioctl, console_poll,
									NULL);
	if (r != 0 && r != -EEXIST)
		return r;

	r = devfs_register_chr_poll("/dev/tty", 0666, console_read, console_write,
								console_ioctl, console_poll, NULL);
	if (r != 0 && r != -EEXIST)
		return r;

	r = devfs_register_chr_poll("/dev/stdin", 0444, console_read, NULL,
								console_ioctl, console_poll, NULL);
	if (r != 0 && r != -EEXIST)
		return r;

	r = devfs_register_chr_poll("/dev/stdout", 0222, NULL, console_write,
								console_ioctl, console_poll, NULL);
	if (r != 0 && r != -EEXIST)
		return r;

	r = devfs_register_chr_poll("/dev/stderr", 0222, NULL, console_write,
								console_ioctl, console_poll, NULL);
	if (r != 0 && r != -EEXIST)
		return r;

	return 0;
}
