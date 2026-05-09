#include <banner.h>
#include <login.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static void write_all(const char *s, size_t len)
{
	while (len > 0) {
		ssize_t w = write(STDOUT_FILENO, s, len);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return;
		}

		s += w;
		len -= (size_t)w;
	}
}

static void write_str(const char *s)
{
	write_all(s, strlen(s));
}

static const char *tty_line(void)
{
	const char *tty = ttyname(STDIN_FILENO);

	if (!tty)
		tty = ttyname(STDOUT_FILENO);

	if (!tty)
		return "tty?";

	if (strncmp(tty, "/dev/", 5) == 0)
		tty += 5;

	return tty;
}

static void write_time_fmt(const char *fmt)
{
	time_t now = time(NULL);
	struct tm tm;
	char buf[64];

	if (now == (time_t)-1)
		return;

	if (!localtime_r(&now, &tm))
		return;

	if (strftime(buf, sizeof(buf), fmt, &tm) == 0)
		return;

	write_str(buf);
}

static void write_issue_escape(char c, const struct utsname *uts)
{
	switch (c) {
	case '\\':
		write_str("\\");
		break;

	case 'n':
		write_str(uts->nodename);
		break;

	case 's':
		write_str(uts->sysname);
		break;

	case 'r':
		write_str(uts->release);
		break;

	case 'v':
		write_str(uts->version);
		break;

	case 'm':
		write_str(uts->machine);
		break;

	case 'l':
		write_str(tty_line());
		break;

	case 'd':
		write_time_fmt("%a %b %e %Y");
		break;

	case 't':
		write_time_fmt("%H:%M:%S");
		break;

	case 'b':
		/*
		 * Baud rate is intentionally unsupported.
		 */
		break;

	case 'o':
		if (uts->__domainname[0] != '\0')
			write_str(uts->__domainname);
		break;

	default:
		/*
		 * Unknown escape: preserve it literally.
		 */
		write_str("\\");
		write_all(&c, 1);
		break;
	}
}

static void print_issue_file(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return;

	struct utsname uts;

	if (uname(&uts) < 0) {
		memset(&uts, 0, sizeof(uts));
		strcpy(uts.sysname, "lyrOS");
		strcpy(uts.nodename, "lyr");
		strcpy(uts.release, "?");
		strcpy(uts.version, "?");
		strcpy(uts.machine, "?");
	}

	char buf[256];
	ssize_t n;
	int escaped = 0;

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			char c = buf[i];

			if (escaped) {
				write_issue_escape(c, &uts);
				escaped = 0;
				continue;
			}

			if (c == '\\') {
				escaped = 1;
				continue;
			}

			write_all(&c, 1);
		}
	}

	if (escaped)
		write_str("\\");

	close(fd);
}

static void print_file(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return;

	char buf[256];
	ssize_t n;

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		char *p = buf;

		while (n > 0) {
			ssize_t w = write(STDOUT_FILENO, p, (size_t)n);

			if (w < 0) {
				if (errno == EINTR)
					continue;

				close(fd);
				return;
			}

			p += w;
			n -= w;
		}
	}

	close(fd);
}

void banner_print_issue(void)
{
	static const char clear_screen[] = "\033[2J\033[H\033[3J";

	write_all(clear_screen, sizeof(clear_screen) - 1);
	print_issue_file(ISSUE_FILE);
}

void banner_print_motd(void)
{
	print_file(MOTD_FILE);
}