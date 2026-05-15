#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef KEY_CNT
#define KEY_CNT 0x300
#endif

#ifndef EV_CNT
#define EV_CNT 0x20
#endif

#define MAX_DEVICES 64
#define BITS_PER_BYTE 8

struct keyboard_state {
	bool left_shift;
	bool right_shift;
	bool caps_lock;
	bool left_ctrl;
	bool right_ctrl;
	bool left_alt;
	bool right_alt;
};

static int test_bit(const unsigned char *bits, unsigned int bit)
{
	return !!(bits[bit / BITS_PER_BYTE] & (1u << (bit % BITS_PER_BYTE)));
}

static const char *key_value_name(int value)
{
	switch (value) {
	case 0:
		return "release";
	case 1:
		return "press";
	case 2:
		return "repeat";
	default:
		return "unknown";
	}
}

static bool is_keyboard_fd(int fd)
{
	unsigned char ev_bits[(EV_CNT + 7) / 8] = { 0 };
	unsigned char key_bits[(KEY_CNT + 7) / 8] = { 0 };

	if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0)
		return false;

	if (!test_bit(ev_bits, EV_KEY))
		return false;

	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
		return false;

	/*
	 * Conservative keyboard heuristic.
	 */
	if (!test_bit(key_bits, KEY_A))
		return false;
	if (!test_bit(key_bits, KEY_Z))
		return false;
	if (!test_bit(key_bits, KEY_1))
		return false;
	if (!test_bit(key_bits, KEY_ENTER))
		return false;
	if (!test_bit(key_bits, KEY_SPACE))
		return false;

	return true;
}

static int open_keyboard(const char *path)
{
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;

	if (!is_keyboard_fd(fd)) {
		close(fd);
		errno = ENODEV;
		return -1;
	}

	return fd;
}

static void print_device_info(int fd, const char *path)
{
	char name[256] = { 0 };
	struct input_id id = { 0 };

	if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
		snprintf(name, sizeof(name), "unknown");

	if (ioctl(fd, EVIOCGID, &id) < 0)
		memset(&id, 0, sizeof(id));

	printf("keyboard: %s\n", path);
	printf("  name:    %s\n", name);
	printf("  bustype: %u\n", id.bustype);
	printf("  vendor:  0x%04x\n", id.vendor);
	printf("  product: 0x%04x\n", id.product);
	printf("  version: 0x%04x\n", id.version);
}

static void update_keyboard_state(struct keyboard_state *st, unsigned int code,
								  int value)
{
	bool down = value != 0;

	switch (code) {
	case KEY_LEFTSHIFT:
		st->left_shift = down;
		break;
	case KEY_RIGHTSHIFT:
		st->right_shift = down;
		break;
	case KEY_LEFTCTRL:
		st->left_ctrl = down;
		break;
	case KEY_RIGHTCTRL:
		st->right_ctrl = down;
		break;
	case KEY_LEFTALT:
		st->left_alt = down;
		break;
	case KEY_RIGHTALT:
		st->right_alt = down;
		break;
	case KEY_CAPSLOCK:
		if (value == 1)
			st->caps_lock = !st->caps_lock;
		break;
	default:
		break;
	}
}

static const char *special_key_name(unsigned int code)
{
	switch (code) {
	case KEY_ENTER:
		return "\\n";
	case KEY_TAB:
		return "\\t";
	case KEY_BACKSPACE:
		return "\\b";
	case KEY_ESC:
		return "ESC";
	case KEY_SPACE:
		return "SPACE";
	default:
		return NULL;
	}
}

static int keycode_to_char(unsigned int code, const struct keyboard_state *st)
{
	bool shift = st->left_shift || st->right_shift;
	bool caps = st->caps_lock;

	/*
	 * US-QWERTY printable translation.
	 * Returns:
	 *   >= 0: printable ASCII char
	 *   -1:  not printable / unsupported
	 */

	if (code >= KEY_A && code <= KEY_Z) {
		char c = 'a' + (code - KEY_A);

		if (shift ^ caps)
			c = (char)(c - 'a' + 'A');

		return c;
	}

	switch (code) {
	case KEY_1:
		return shift ? '!' : '1';
	case KEY_2:
		return shift ? '@' : '2';
	case KEY_3:
		return shift ? '#' : '3';
	case KEY_4:
		return shift ? '$' : '4';
	case KEY_5:
		return shift ? '%' : '5';
	case KEY_6:
		return shift ? '^' : '6';
	case KEY_7:
		return shift ? '&' : '7';
	case KEY_8:
		return shift ? '*' : '8';
	case KEY_9:
		return shift ? '(' : '9';
	case KEY_0:
		return shift ? ')' : '0';

	case KEY_MINUS:
		return shift ? '_' : '-';
	case KEY_EQUAL:
		return shift ? '+' : '=';
	case KEY_LEFTBRACE:
		return shift ? '{' : '[';
	case KEY_RIGHTBRACE:
		return shift ? '}' : ']';
	case KEY_BACKSLASH:
		return shift ? '|' : '\\';
	case KEY_SEMICOLON:
		return shift ? ':' : ';';
	case KEY_APOSTROPHE:
		return shift ? '"' : '\'';
	case KEY_GRAVE:
		return shift ? '~' : '`';
	case KEY_COMMA:
		return shift ? '<' : ',';
	case KEY_DOT:
		return shift ? '>' : '.';
	case KEY_SLASH:
		return shift ? '?' : '/';
	case KEY_SPACE:
		return ' ';

	default:
		return -1;
	}
}

static void print_char_repr(int ch)
{
	if (ch < 0) {
		printf("char=<none>");
		return;
	}

	switch (ch) {
	case '\n':
		printf("char='\\n'");
		break;
	case '\t':
		printf("char='\\t'");
		break;
	case '\b':
		printf("char='\\b'");
		break;
	case ' ':
		printf("char=' '");
		break;
	case '\\':
		printf("char='\\\\'");
		break;
	case '\'':
		printf("char='\\''");
		break;
	default:
		if (ch >= 32 && ch <= 126)
			printf("char='%c'", ch);
		else
			printf("char=0x%02x", ch & 0xff);
		break;
	}
}

int main(void)
{
	glob_t g;
	struct pollfd pfds[MAX_DEVICES];
	const char *paths[MAX_DEVICES];
	struct keyboard_state states[MAX_DEVICES];
	int count = 0;

	memset(&g, 0, sizeof(g));
	memset(pfds, 0, sizeof(pfds));
	memset(paths, 0, sizeof(paths));
	memset(states, 0, sizeof(states));

	if (glob("/dev/input/event*", 0, NULL, &g) != 0) {
		fprintf(stderr, "no /dev/input/event* devices found\n");
		return 1;
	}

	for (size_t i = 0; i < g.gl_pathc && count < MAX_DEVICES; i++) {
		const char *path = g.gl_pathv[i];
		int fd = open_keyboard(path);

		if (fd < 0)
			continue;

		pfds[count].fd = fd;
		pfds[count].events = POLLIN;
		paths[count] = strdup(path);

		if (!paths[count]) {
			close(fd);
			continue;
		}

		print_device_info(fd, path);
		count++;
	}

	globfree(&g);

	if (count == 0) {
		fprintf(stderr, "no keyboards found under /dev/input/event*\n");
		return 1;
	}

	printf("reading keyboard input forever...\n");

	for (;;) {
		int rc = poll(pfds, count, -1);
		if (rc < 0) {
			if (errno == EINTR)
				continue;

			perror("poll");
			break;
		}

		for (int i = 0; i < count; i++) {
			if (!(pfds[i].revents & POLLIN))
				continue;

			for (;;) {
				struct input_event ev;
				ssize_t n = read(pfds[i].fd, &ev, sizeof(ev));

				if (n < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						break;

					perror("read");
					break;
				}

				if (n == 0)
					break;

				if (n != sizeof(ev)) {
					fprintf(stderr, "%s: short read: %zd bytes\n", paths[i], n);
					break;
				}

				if (ev.type != EV_KEY)
					continue;

				/*
				 * Update modifiers before translating normal key presses.
				 */
				update_keyboard_state(&states[i], ev.code, ev.value);

				int ch = -1;
				const char *special = NULL;

				if (ev.value == 1 || ev.value == 2) {
					ch = keycode_to_char(ev.code, &states[i]);
					special = special_key_name(ev.code);
				}

				printf("%s: key code=%u value=%d (%s) ", paths[i], ev.code,
					   ev.value, key_value_name(ev.value));

				if (special && ch < 0)
					printf("char=%s", special);
				else
					print_char_repr(ch);

				printf(" shift=%d caps=%d ctrl=%d alt=%d\n",
					   states[i].left_shift || states[i].right_shift,
					   states[i].caps_lock,
					   states[i].left_ctrl || states[i].right_ctrl,
					   states[i].left_alt || states[i].right_alt);

				fflush(stdout);
			}
		}
	}

	for (int i = 0; i < count; i++) {
		close(pfds[i].fd);
		free((void *)paths[i]);
	}

	return 1;
}