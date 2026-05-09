#include <term.h>
#include <login.h>

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define READ_ECHO_NORMAL 0
#define READ_ECHO_PASSWORD 1

static void erase_one(void)
{
	const char seq[] = { '\b', ' ', '\b' };
	write(STDOUT_FILENO, seq, sizeof(seq));
}

static int read_raw_line(const char *prompt, char *buf, size_t size,
						 int echo_mode)
{
	struct termios oldt, newt;
	int have_termios = 0;
	size_t pos = 0;

	if (prompt)
		write(STDOUT_FILENO, prompt, strlen(prompt));

	if (size == 0)
		return -1;

	if (tcgetattr(STDIN_FILENO, &oldt) == 0) {
		newt = oldt;
		newt.c_lflag &= ~(tcflag_t)(ECHO | ICANON);
		newt.c_cc[VMIN] = 1;
		newt.c_cc[VTIME] = 0;
		if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &newt) == 0)
			have_termios = 1;
	}

	for (;;) {
		char c;
		ssize_t n = read(STDIN_FILENO, &c, 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (have_termios)
				tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
			return -1;
		}
		if (n == 0)
			continue;

		if (c == '\n' || c == '\r')
			break;

		if (c == '\b' || c == 8 || c == 127) {
			if (pos > 0) {
				buf[--pos] = '\0';
				erase_one();
			}
			continue;
		}

		if ((unsigned char)c < 32)
			continue;

		if (pos + 1 >= size)
			continue;

		buf[pos++] = c;
		buf[pos] = '\0';

		if (echo_mode == READ_ECHO_PASSWORD)
			write(STDOUT_FILENO, "*", 1);
		else
			write(STDOUT_FILENO, &c, 1);
	}

	buf[pos] = '\0';
	if (have_termios)
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
	write(STDOUT_FILENO, "\n", 1);
	return 0;
}

void term_clear(void)
{
}

void term_println(const char *ansi_code, const char *msg)
{
	(void)ansi_code;
	write(STDOUT_FILENO, msg, strlen(msg));
	write(STDOUT_FILENO, "\n", 1);
}

void term_clear_string(char *s)
{
	if (!s)
		return;
	volatile char *p = s;
	while (*p)
		*p++ = '\0';
}

void term_trim_newline(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
		s[--n] = '\0';
}

void term_trim_spaces(char *s)
{
	size_t start = 0;
	while (s[start] == ' ' || s[start] == '\t')
		start++;
	if (start)
		memmove(s, s + start, strlen(s + start) + 1);
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';
}

int term_read_line(const char *prompt, char *buf, size_t size)
{
	return read_raw_line(prompt, buf, size, READ_ECHO_NORMAL);
}

int term_read_password(const char *prompt, char *buf, size_t size)
{
	return read_raw_line(prompt, buf, size, READ_ECHO_PASSWORD);
}
