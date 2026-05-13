#include <dev/console.h>
#include <dev/uart.h>
#include <errno.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <lib/lyrterm.h>
#include <lib/string.h>
#include <sched/sched.h>
#include <sys/poll.h>

#define LYR_NCCS 32

/* c_lflag */
#define LYR_ISIG 0000001u
#define LYR_ICANON 0000002u
#define LYR_ECHO 0000010u
#define LYR_ECHOE 0000020u
#define LYR_ECHOK 0000040u
#define LYR_ECHONL 0000100u
#define LYR_NOFLSH 0000200u
#define LYR_IEXTEN 0100000u

/* c_iflag */
#define LYR_IGNBRK 0000001u
#define LYR_BRKINT 0000002u
#define LYR_IGNPAR 0000004u
#define LYR_PARMRK 0000010u
#define LYR_INPCK 0000020u
#define LYR_ISTRIP 0000040u
#define LYR_INLCR 0000100u
#define LYR_IGNCR 0000200u
#define LYR_ICRNL 0000400u
#define LYR_IXON 0002000u
#define LYR_IXOFF 0010000u

/* c_oflag */
#define LYR_OPOST 0000001u
#define LYR_ONLCR 0000004u

/* c_cc indexes */
#define LYR_VINTR 0
#define LYR_VQUIT 1
#define LYR_VERASE 2
#define LYR_VKILL 3
#define LYR_VEOF 4
#define LYR_VTIME 5
#define LYR_VMIN 6
#define LYR_VSTART 8
#define LYR_VSTOP 9
#define LYR_VSUSP 10

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
	pid_t foreground_pgrp;
	volatile uint8_t input_interrupted;
	lyrterm_state_t render;
} console_tty_t;

typedef enum console_target_kind {
	CONSOLE_TARGET_ACTIVE = 0,
	CONSOLE_TARGET_PROCESS_TTY,
} console_target_kind_t;

typedef struct console_target {
	console_target_kind_t kind;
} console_target_t;

static console_tty_t consoles[LYR_TTY_COUNT];
static unsigned active_console;

static const console_target_t console_target_active = {
	.kind = CONSOLE_TARGET_ACTIVE,
};

static const console_target_t console_target_process_tty = {
	.kind = CONSOLE_TARGET_PROCESS_TTY,
};

static int console_write(void *ctx, uint64_t off, const void *buf, size_t len,
						 size_t *done);

static console_tty_t *console_ctx_to_tty(void *ctx)
{
	if (!ctx)
		return &consoles[active_console];

	const console_target_t *target = (const console_target_t *)ctx;

	if (target == &console_target_active)
		return &consoles[active_console];

	if (target == &console_target_process_tty) {
		tcb_t *thread = sched_current();
		if (thread && thread->process) {
			int tty_index = thread->process->controlling_tty;
			if (tty_index >= 0 && tty_index < (int)LYR_TTY_COUNT)
				return &consoles[tty_index];
		}

		return &consoles[active_console];
	}

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

	/*
	 * Default cooked terminal behavior:
	 *
	 * Input:
	 *   ICRNL maps keyboard Enter '\r' to userspace '\n' in canonical mode.
	 *
	 * Output:
	 *   OPOST|ONLCR maps userspace '\n' to terminal "\r\n".
	 *
	 * Local:
	 *   ICANON/ECHO/ISIG for normal shell behavior.
	 */
	tty->termios.c_iflag = LYR_ICRNL | LYR_IXON;
	tty->termios.c_oflag = LYR_OPOST | LYR_ONLCR;
	tty->termios.c_lflag =
		LYR_ISIG | LYR_ICANON | LYR_ECHO | LYR_ECHOE | LYR_ECHOK | LYR_IEXTEN;

	tty->termios.c_ispeed = 38400;
	tty->termios.c_ospeed = 38400;

	tty->termios.c_cc[LYR_VINTR] = 3; /* ^C */
	tty->termios.c_cc[LYR_VQUIT] = 28; /* ^\ */
	tty->termios.c_cc[LYR_VERASE] = 127;
	tty->termios.c_cc[LYR_VKILL] = 21; /* ^U */
	tty->termios.c_cc[LYR_VEOF] = 4; /* ^D */
	tty->termios.c_cc[LYR_VMIN] = 1;
	tty->termios.c_cc[LYR_VTIME] = 0;
	tty->termios.c_cc[LYR_VSTART] = 17; /* ^Q */
	tty->termios.c_cc[LYR_VSTOP] = 19; /* ^S */
	tty->termios.c_cc[LYR_VSUSP] = 26; /* ^Z */

	tty->foreground_pgrp = 0;

	console_tty_name(index, tty->name, sizeof(tty->name));
	lyrterm_capture_state(&tty->render);
}

static void console_input_flush(console_tty_t *tty)
{
	if (!tty)
		return;

	memset(&tty->input, 0, sizeof(tty->input));
}

static int console_wait_input(console_tty_t *tty)
{
	if (tty->index == active_console)
		lyrterm_flush();

	for (;;) {
		if (tty->input_interrupted) {
			tty->input_interrupted = 0;
			return -EINTR;
		}

		tcb_t *thread = sched_current();
		if (thread && sched_signal_is_pending(thread))
			return -EINTR;

		if (tty->termios.c_lflag & LYR_ICANON) {
			if (tty->input.lines > 0)
				return 0;
		} else if (tty->input.count > 0) {
			return 0;
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

		if (!utf8_is_continuation(old) || in->count == 0 ||
			in->tail == in->head)
			break;

		prev = in->head ? in->head - 1 : CONSOLE_INPUT_SIZE - 1;
		old = in->buf[prev];

		if (old == '\n' || old == '\r')
			break;
	}

	if ((tty->termios.c_lflag & LYR_ECHO) && tty->index == active_console)
		console_write(tty, 0, "\b \b", 3, NULL);
}

static void console_raise_signal(console_tty_t *tty, int signal)
{
	if (!tty || tty->foreground_pgrp <= 0)
		return;

	if (!(tty->termios.c_lflag & LYR_NOFLSH))
		console_input_flush(tty);

	(void)sched_process_signal_group(tty->foreground_pgrp, signal);
}

static int console_is_line_delimiter(uint8_t ch)
{
	return ch == '\n' || ch == '\r';
}

static void console_echo_input(console_tty_t *tty, uint8_t ch)
{
	if (!(tty->termios.c_lflag & LYR_ECHO))
		return;

	if (tty->index != active_console)
		return;

	if (ch == '\n') {
		console_write(tty, 0, "\r\n", 2, NULL);
	} else if (ch == '\r') {
		console_write(tty, 0, "\r", 1, NULL);
	} else if (ch == '\t') {
		console_write(tty, 0, "\t", 1, NULL);
	} else if (ch < 0x20) {
		char caret[2] = { '^', (char)(ch + 0x40) };
		console_write(tty, 0, caret, 2, NULL);
	} else if (ch == 0x7f) {
		console_write(tty, 0, "^?", 2, NULL);
	} else {
		console_write(tty, 0, &ch, 1, NULL);
	}
}

static int console_input_translate(console_tty_t *tty, uint8_t *chp)
{
	uint8_t ch = *chp;

	if (ch == '\r') {
		if (tty->termios.c_iflag & LYR_IGNCR)
			return 0;

		if (tty->termios.c_iflag & LYR_ICRNL)
			ch = '\n';
	} else if (ch == '\n') {
		if (tty->termios.c_iflag & LYR_INLCR)
			ch = '\r';
	}

	if (tty->termios.c_iflag & LYR_ISTRIP)
		ch &= 0x7f;

	*chp = ch;
	return 1;
}

void console_input_put(uint8_t ch)
{
	console_tty_t *tty = &consoles[active_console];

	if (!console_input_translate(tty, &ch))
		return;

	int canonical = (tty->termios.c_lflag & LYR_ICANON) != 0;
	int echo = (tty->termios.c_lflag & LYR_ECHO) != 0;

	if (tty->termios.c_lflag & LYR_ISIG) {
		if (ch == tty->termios.c_cc[LYR_VINTR]) {
			if (echo && tty->index == active_console)
				console_write(tty, 0, "^C\r\n", 4, NULL);

			tty->input_interrupted = 1;
			console_raise_signal(tty, 2);
			return;
		}

		if (ch == tty->termios.c_cc[LYR_VQUIT]) {
			if (echo && tty->index == active_console)
				console_write(tty, 0, "^\\\r\n", 5, NULL);

			tty->input_interrupted = 1;
			console_raise_signal(tty, 3);
			return;
		}
	}

	if (canonical &&
		(ch == 8 || ch == 127 || ch == tty->termios.c_cc[LYR_VERASE])) {
		console_input_backspace(tty);
		return;
	}

	/*
	 * Canonical EOF (^D): wake readers without placing the EOF byte into
	 * the stream. If the line is empty, read() returns 0 bytes.
	 */
	if (canonical && ch == tty->termios.c_cc[LYR_VEOF]) {
		tty->input.lines++;
		return;
	}

	console_input_push(tty, ch);
	console_echo_input(tty, ch);
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

	tcb_t *thread = sched_current();

	if (thread && thread->process && thread->process->vas &&
		(uint64_t)(uintptr_t)buf < VAS_USER_END &&
		vas_user_access_ok(thread->process->vas, (uint64_t)(uintptr_t)buf, len,
						   1) != 0)
		return -EFAULT;

	int wr = console_wait_input(tty);
	if (wr != 0)
		return wr;

	uint8_t *p = buf;
	size_t count = 0;
	int canonical = (tty->termios.c_lflag & LYR_ICANON) != 0;

	while (count < len) {
		uint8_t ch;

		if (!console_input_pop(tty, &ch))
			break;

		p[count++] = ch;

		if (canonical && console_is_line_delimiter(ch))
			break;
	}

	if (done)
		*done = count;

	return 0;
}

static void console_write_translated(console_tty_t *tty, const uint8_t *buf,
									 size_t len)
{
	if (!(tty->termios.c_oflag & LYR_OPOST)) {
		if (tty->index == active_console) {
			uart_wbuf((const char *)buf, len);
			lyrterm_write((const char *)buf, len);
		} else {
			lyrterm_update_state(&tty->render, (const char *)buf, len);
		}
		return;
	}

	for (size_t i = 0; i < len; i++) {
		uint8_t ch = buf[i];

		if ((tty->termios.c_oflag & LYR_ONLCR) && ch == '\n') {
			const char crlf[2] = { '\r', '\n' };

			if (tty->index == active_console) {
				uart_wbuf(crlf, 2);
				lyrterm_write(crlf, 2);
			} else {
				lyrterm_update_state(&tty->render, crlf, 2);
			}
		} else {
			if (tty->index == active_console) {
				uart_wbuf((const char *)&ch, 1);
				lyrterm_write((const char *)&ch, 1);
			} else {
				lyrterm_update_state(&tty->render, (const char *)&ch, 1);
			}
		}
	}
}

static int console_write(void *ctx, uint64_t off, const void *buf, size_t len,
						 size_t *done)
{
	(void)off;

	console_tty_t *tty = console_ctx_to_tty(ctx);

	if (!buf)
		return -EINVAL;

	console_write_translated(tty, (const uint8_t *)buf, len);

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
	tcb_t *thread = sched_current();
	pcb_t *process = thread ? thread->process : NULL;

#define CONSOLE_USER_ARG_OK(size, write)                                    \
	((arg) && (!process || !process->vas ||                                 \
			   (uint64_t)(uintptr_t)(arg) >= VAS_USER_END ||                \
			   vas_user_access_ok(process->vas, (uint64_t)(uintptr_t)(arg), \
								  (size), (write)) == 0))

	switch (request) {
	case LYR_TCGETS:
		if (!CONSOLE_USER_ARG_OK(sizeof(tty->termios), 1))
			return arg ? -EFAULT : -EINVAL;

		memcpy(arg, &tty->termios, sizeof(tty->termios));
		return 0;

	case LYR_TCSETS:
	case LYR_TCSETSW:
		if (!CONSOLE_USER_ARG_OK(sizeof(tty->termios), 0))
			return arg ? -EFAULT : -EINVAL;

		memcpy(&tty->termios, arg, sizeof(tty->termios));
		return 0;

	case LYR_TCSETSF:
		if (!CONSOLE_USER_ARG_OK(sizeof(tty->termios), 0))
			return arg ? -EFAULT : -EINVAL;

		memcpy(&tty->termios, arg, sizeof(tty->termios));
		console_input_flush(tty);
		return 0;

	case LYR_TIOCGWINSZ:
		if (!CONSOLE_USER_ARG_OK(sizeof(lyr_winsize_t), 1))
			return arg ? -EFAULT : -EINVAL;

		return console_get_winsize((lyr_winsize_t *)arg);

	case LYR_TIOCSWINSZ:
		return arg ? 0 : -EINVAL;

	case LYR_TIOCGNAME:
		if (!CONSOLE_USER_ARG_OK(strlen(tty->name) + 1, 1))
			return arg ? -EFAULT : -EINVAL;

		memcpy(arg, tty->name, strlen(tty->name) + 1);
		return 0;

	case LYR_TIOCGPGRP:
		if (!CONSOLE_USER_ARG_OK(sizeof(pid_t), 1))
			return arg ? -EFAULT : -EINVAL;

		*(pid_t *)arg = tty->foreground_pgrp;
		return 0;

	case LYR_TIOCSPGRP: {
		pid_t pgid;
		pcb_t *process = NULL;
		tcb_t *thread = sched_current();

		if (!arg)
			return -EINVAL;

		if (!thread || !thread->process)
			return -EBADF;

		pgid = *(pid_t *)arg;
		process = thread->process;

		if (pgid <= 0)
			return -EINVAL;

		if (process->sid <= 0)
			return -EPERM;

		tty->foreground_pgrp = pgid;
		process->controlling_tty = (int)tty->index;
		return 0;
	}

	default:
		return -ENOTTY;
	}

#undef CONSOLE_USER_ARG_OK
}

int console_switch_tty(unsigned index)
{
	if (index >= LYR_TTY_COUNT || index == active_console)
		return index < LYR_TTY_COUNT ? 0 : -EINVAL;

	lyrterm_flush();
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

		int r =
			devfs_register_chr_poll(path, 0666, console_read, console_write,
									console_ioctl, console_poll, &consoles[i]);

		if (r != 0 && r != -EEXIST)
			return r;
	}

	int r = devfs_register_chr_poll("/dev/console", 0666, console_read,
									console_write, console_ioctl, console_poll,
									(void *)&console_target_active);
	if (r != 0 && r != -EEXIST)
		return r;

	r = devfs_register_chr_poll("/dev/tty", 0666, console_read, console_write,
								console_ioctl, console_poll,
								(void *)&console_target_process_tty);
	if (r != 0 && r != -EEXIST)
		return r;

	r = devfs_register_chr_poll("/dev/stdin", 0444, console_read, NULL,
								console_ioctl, console_poll,
								(void *)&console_target_process_tty);
	if (r != 0 && r != -EEXIST)
		return r;

	r = devfs_register_chr_poll("/dev/stdout", 0222, NULL, console_write,
								console_ioctl, console_poll,
								(void *)&console_target_process_tty);
	if (r != 0 && r != -EEXIST)
		return r;

	r = devfs_register_chr_poll("/dev/stderr", 0222, NULL, console_write,
								console_ioctl, console_poll,
								(void *)&console_target_process_tty);
	if (r != 0 && r != -EEXIST)
		return r;

	return 0;
}