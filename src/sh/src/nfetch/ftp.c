#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sh.h>
#include "scheme.h"

static int ftp_write_cmd(int s, const char *cmd)
{
	if (nfetch_sock_write_all(s, cmd, strlen(cmd)) < 0)
		return -1;

	return nfetch_sock_write_all(s, "\r\n", 2);
}

static int ftp_read_line(int s, char *buf, size_t bufsz, int timeout_sec)
{
	size_t n = 0;

	nfetch_set_recv_timeout(s, timeout_sec);

	while (n + 1 < bufsz) {
		char c;
		ssize_t r = recv(s, &c, 1, 0);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		if (r == 0) {
			if (n == 0)
				return 0;
			break;
		}

		buf[n++] = c;

		if (c == '\n')
			break;
	}

	buf[n] = '\0';
	return (int)n;
}

static int ftp_code_from_line(const char *line)
{
	if (!isdigit((unsigned char)line[0]) ||
		!isdigit((unsigned char)line[1]) ||
		!isdigit((unsigned char)line[2]))
		return 0;

	return (line[0] - '0') * 100 + (line[1] - '0') * 10 +
		(line[2] - '0');
}

static int ftp_read_reply(int s, int outfd, int timeout_sec, int echo,
					  char *last, size_t lastsz)
{
	char line[2048];
	int code = 0;
	int multiline = 0;

	for (;;) {
		int n = ftp_read_line(s, line, sizeof(line), timeout_sec);

		if (n < 0) {
			fprintf(stderr, "nfetch: ftp: recv: %s\n", strerror(errno));
			return -1;
		}

		if (n == 0) {
			fprintf(stderr, "nfetch: ftp: control connection closed\n");
			return -1;
		}

		if (last && lastsz) {
			strncpy(last, line, lastsz - 1);
			last[lastsz - 1] = '\0';
		}

		if (echo && nfetch_write_all(outfd, line, (size_t)n) < 0) {
			fprintf(stderr, "nfetch: write: %s\n", strerror(errno));
			return -1;
		}

		if (!code) {
			code = ftp_code_from_line(line);

			if (!code)
				continue;

			if (line[3] == '-') {
				multiline = 1;
				continue;
			}

			return code;
		}

		if (multiline && ftp_code_from_line(line) == code && line[3] == ' ')
			return code;
	}
}


static int ftp_login(int s, const nfetch_opts_t *opts, int outfd, int echo)
{
	const char *user = opts->username ? opts->username : "anonymous";
	const char *pass = opts->password ? opts->password : "nfetch@";
	char cmd[1024];
	int code;

	code = ftp_read_reply(s, outfd, 0, echo, NULL, 0);
	if (code != 220)
		return 1;

	if (snprintf(cmd, sizeof(cmd), "USER %s", user) >= (int)sizeof(cmd)) {
		fprintf(stderr, "nfetch: ftp: username too large\n");
		return 1;
	}

	if (ftp_write_cmd(s, cmd) < 0) {
		fprintf(stderr, "nfetch: ftp: write USER: %s\n", strerror(errno));
		return 1;
	}

	code = ftp_read_reply(s, outfd, 0, echo, NULL, 0);

	if (code == 331) {
		if (snprintf(cmd, sizeof(cmd), "PASS %s", pass) >= (int)sizeof(cmd)) {
			fprintf(stderr, "nfetch: ftp: password too large\n");
			return 1;
		}

		if (ftp_write_cmd(s, cmd) < 0) {
			fprintf(stderr, "nfetch: ftp: write PASS: %s\n", strerror(errno));
			return 1;
		}

		code = ftp_read_reply(s, outfd, 0, echo, NULL, 0);
	}

	if (code != 230 && code != 202) {
		fprintf(stderr, "nfetch: ftp: login failed, reply %d\n", code);
		return 1;
	}

	return 0;
}

static int ftp_parse_pasv(const char *line, char *host, size_t hostsz,
					  char *port, size_t portsz)
{
	const char *p = strchr(line, '(');
	int h1, h2, h3, h4, p1, p2;

	if (!p)
		return -1;

	if (sscanf(p + 1, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6)
		return -1;

	if (h1 < 0 || h1 > 255 || h2 < 0 || h2 > 255 || h3 < 0 || h3 > 255 ||
		h4 < 0 || h4 > 255 || p1 < 0 || p1 > 255 || p2 < 0 || p2 > 255)
		return -1;

	snprintf(host, hostsz, "%d.%d.%d.%d", h1, h2, h3, h4);
	snprintf(port, portsz, "%d", p1 * 256 + p2);
	return 0;
}

static int ftp_parse_epsv(const char *line, char *port, size_t portsz)
{
	const char *p = strchr(line, '(');
	char delim;
	const char *a;
	const char *b;
	const char *c;

	if (!p || !p[1])
		return -1;

	/* RFC 2428: 229 Entering Extended Passive Mode (|||6446|)
	 * The delimiter is arbitrary; the TCP port is between the third and
	 * fourth delimiter. EPSV avoids trusting a possibly wrong PASV address.
	 */
	delim = p[1];
	a = strchr(p + 2, delim);
	if (!a)
		return -1;
	b = strchr(a + 1, delim);
	if (!b)
		return -1;
	c = strchr(b + 1, delim);
	if (!c || c == b + 1)
		return -1;

	if ((size_t)(c - (b + 1)) >= portsz)
		return -1;

	memcpy(port, b + 1, (size_t)(c - (b + 1)));
	port[c - (b + 1)] = '\0';
	return 0;
}

static int ftp_connect_addr_timeout(int s, const struct sockaddr *addr,
					    socklen_t addrlen, int timeout_sec)
{
	int flags;
	int err = 0;
	socklen_t errlen = sizeof(err);
	fd_set wfds;
	struct timeval tv;
	int r;

	if (timeout_sec <= 0)
		timeout_sec = 5;

	flags = fcntl(s, F_GETFL, 0);
	if (flags < 0)
		return -1;

	if (fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;

	r = connect(s, addr, addrlen);
	if (r == 0) {
		(void)fcntl(s, F_SETFL, flags);
		return 0;
	}

	if (errno != EINPROGRESS) {
		(void)fcntl(s, F_SETFL, flags);
		return -1;
	}

	FD_ZERO(&wfds);
	FD_SET(s, &wfds);
	tv.tv_sec = timeout_sec;
	tv.tv_usec = 0;

	do {
		r = select(s + 1, NULL, &wfds, NULL, &tv);
	} while (r < 0 && errno == EINTR);

	if (r == 0) {
		errno = ETIMEDOUT;
		(void)fcntl(s, F_SETFL, flags);
		return -1;
	}

	if (r < 0) {
		(void)fcntl(s, F_SETFL, flags);
		return -1;
	}

	if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
		(void)fcntl(s, F_SETFL, flags);
		return -1;
	}

	if (err) {
		errno = err;
		(void)fcntl(s, F_SETFL, flags);
		return -1;
	}

	return fcntl(s, F_SETFL, flags);
}

static int ftp_connect_control_peer_port(int ctrl, const char *portstr,
						 int verbose, int timeout_sec)
{
	struct sockaddr_storage ss;
	socklen_t slen = sizeof(ss);
	int port = atoi(portstr);
	int data = -1;
	char addrbuf[INET6_ADDRSTRLEN];

	if (port <= 0 || port > 65535) {
		fprintf(stderr, "nfetch: ftp: bad EPSV port: %s\n", portstr);
		return -1;
	}

	if (getpeername(ctrl, (struct sockaddr *)&ss, &slen) < 0) {
		fprintf(stderr, "nfetch: ftp: getpeername: %s\n", strerror(errno));
		return -1;
	}

	if (ss.ss_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
		sin->sin_port = htons((unsigned short)port);
		data = socket(AF_INET, SOCK_STREAM, 0);
		if (data < 0) {
			fprintf(stderr, "nfetch: ftp: socket: %s\n", strerror(errno));
			return -1;
		}
		if (ftp_connect_addr_timeout(data, (struct sockaddr *)sin, sizeof(*sin),
						 timeout_sec) == 0) {
			if (verbose) {
				inet_ntop(AF_INET, &sin->sin_addr, addrbuf, sizeof(addrbuf));
				fprintf(stderr, "nfetch: ftp: data connected to %s:%d\n", addrbuf, port);
			}
			return data;
		}
	} else if (ss.ss_family == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
		sin6->sin6_port = htons((unsigned short)port);
		data = socket(AF_INET6, SOCK_STREAM, 0);
		if (data < 0) {
			fprintf(stderr, "nfetch: ftp: socket: %s\n", strerror(errno));
			return -1;
		}
		if (ftp_connect_addr_timeout(data, (struct sockaddr *)sin6, sizeof(*sin6),
						 timeout_sec) == 0) {
			if (verbose) {
				inet_ntop(AF_INET6, &sin6->sin6_addr, addrbuf, sizeof(addrbuf));
				fprintf(stderr, "nfetch: ftp: data connected to [%s]:%d\n", addrbuf, port);
			}
			return data;
		}
	} else {
		fprintf(stderr, "nfetch: ftp: unsupported control address family\n");
		return -1;
	}

	fprintf(stderr, "nfetch: ftp: data connect: %s\n", strerror(errno));
	if (data >= 0)
		close(data);
	return -1;
}

static int ftp_open_passive_data(int ctrl, const nfetch_opts_t *opts,
							 const char *control_host, int outfd, int echo)
{
	char last[2048];
	char host[64];
	char port[16];
	int code;
	int data;

	/* Use classic PASV first. Some public FTP servers advertise EPSV and return
	 * 150/226 on the control channel, but never deliver payload on the EPSV data
	 * socket. ftp.scene.org is one such compatibility case. PASV is the common
	 * denominator for plain IPv4 FTP and is what traditional clients try first.
	 */
	if (ftp_write_cmd(ctrl, "PASV") == 0) {
		code = ftp_read_reply(ctrl, outfd, 0, echo, last, sizeof(last));
		if (code == 227 && ftp_parse_pasv(last, host, sizeof(host), port, sizeof(port)) == 0) {
			data = nfetch_connect_tcp_timeout(host, port, opts->verbose, opts->timeout_sec);
			if (data >= 0)
				return data;
		} else if (opts->verbose) {
			fprintf(stderr, "nfetch: ftp: PASV unavailable, falling back to EPSV\n");
		}
	} else {
		fprintf(stderr, "nfetch: ftp: write PASV: %s\n", strerror(errno));
	}

	if (ftp_write_cmd(ctrl, "EPSV") < 0) {
		fprintf(stderr, "nfetch: ftp: write EPSV: %s\n", strerror(errno));
		return -1;
	}

	code = ftp_read_reply(ctrl, outfd, 0, echo, last, sizeof(last));
	if (code != 229) {
		fprintf(stderr, "nfetch: ftp: EPSV failed, reply %d\n", code);
		return -1;
	}

	if (ftp_parse_epsv(last, port, sizeof(port)) < 0) {
		fprintf(stderr, "nfetch: ftp: could not parse EPSV reply: %s", last);
		return -1;
	}

	(void)control_host;
	return ftp_connect_control_peer_port(ctrl, port, opts->verbose, opts->timeout_sec);
}

static int ftp_set_type(int ctrl, const nfetch_opts_t *opts, const char *type,
						int outfd, int echo)
{
	char cmd[16];
	(void)opts;
	int code;

	snprintf(cmd, sizeof(cmd), "TYPE %s", type);

	if (ftp_write_cmd(ctrl, cmd) < 0) {
		fprintf(stderr, "nfetch: ftp: write TYPE: %s\n", strerror(errno));
		return 1;
	}

	code = ftp_read_reply(ctrl, outfd, 0, echo, NULL, 0);
	return code == 200 ? 0 : 1;
}

static unsigned long ftp_parse_size_from_reply(const char *line)
{
	const char *p;
	unsigned long size;

	if (!line)
		return 0;

	p = strchr(line, '(');
	if (p && sscanf(p, "(%lu bytes", &size) == 1)
		return size;

	return 0;
}

static void ftp_progress_update(const char *label, unsigned long done,
							 unsigned long total, int final)
{
	if (total > 0) {
		unsigned int pct = (unsigned int)((done * 100UL) / total);
		unsigned int fill;
		char bar[21];

		if (pct > 100)
			pct = 100;

		fill = pct / 5;
		for (unsigned int i = 0; i < 20; i++)
			bar[i] = i < fill ? '#' : '-';
		bar[20] = '\0';

		fprintf(stderr, "\r%s [%s] %3u%% %lu/%lu bytes%s", label, bar,
				pct, done, total, final ? "\n" : "");
	} else {
		fprintf(stderr, "\r%s %lu bytes%s", label, done, final ? "\n" : "");
	}
	fflush(stderr);
}

static int ftp_copy_data(int data, int outfd, const nfetch_opts_t *opts,
						 const char *progress_label, unsigned long expected)
{
	char buf[8192];
	(void)opts;
	unsigned long total = 0;
	int show_progress = progress_label && outfd != STDOUT_FILENO;

	if (show_progress)
		ftp_progress_update(progress_label, 0, expected, 0);

	/* FTP data transfers are delimited by EOF on the data socket, or by the
	 * advertised byte count for RETR. Do not use read timeouts here: a timeout
	 * makes successful small LIST/RETR transfers appear to hang before the prompt
	 * returns. Only connect operations are timeout-bound.
	 */
	nfetch_set_recv_timeout(data, 0);

	for (;;) {
		ssize_t n;

		if (expected > 0 && total >= expected)
			break;

		n = recv(data, buf, sizeof(buf), 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;

			fprintf(stderr, "nfetch: recv: %s\n", strerror(errno));
			if (show_progress)
				ftp_progress_update(progress_label, total, expected, 1);
			return 1;
		}

		if (n == 0)
			break;

		if (nfetch_write_all(outfd, buf, (size_t)n) < 0) {
			fprintf(stderr, "nfetch: write: %s\n", strerror(errno));
			if (show_progress)
				ftp_progress_update(progress_label, total, expected, 1);
			return 1;
		}

		total += (unsigned long)n;
		if (show_progress)
			ftp_progress_update(progress_label, total, expected, 0);

		/* If the server told us the transfer size, do not perform one more timed
		 * recv() after the last byte.  That extra recv() is what made tiny RETR
		 * downloads appear to finish and then stall before returning to the prompt.
		 */
		if (expected > 0 && total >= expected)
			break;

	}

	if (show_progress)
		ftp_progress_update(progress_label, total, expected, 1);

	if (expected > 0 && total < expected) {
		fprintf(stderr, "nfetch: ftp: short transfer: got %lu of %lu bytes\n",
			total, expected);
		return 1;
	}

	return 0;
}

static int ftp_transfer(int ctrl, const nfetch_opts_t *opts, const char *control_host,
					int datafd, int ctrlfd, const char *cmd, const char *type,
					int echo, const char *progress_label)
{
	int data;
	int code;
	int status;
	char last[2048];
	unsigned long expected = 0;

	if (ftp_set_type(ctrl, opts, type, ctrlfd, echo) != 0)
		return 1;

	data = ftp_open_passive_data(ctrl, opts, control_host, ctrlfd, echo);
	if (data < 0)
		return 1;

	if (ftp_write_cmd(ctrl, cmd) < 0) {
		fprintf(stderr, "nfetch: ftp: write %s: %s\n", cmd, strerror(errno));
		close(data);
		return 1;
	}

	code = ftp_read_reply(ctrl, ctrlfd, 0, echo, last, sizeof(last));
	if (code != 125 && code != 150) {
		fprintf(stderr, "nfetch: ftp: transfer not started, reply %d\n", code);
		close(data);
		return 1;
	}

	expected = ftp_parse_size_from_reply(last);
	status = ftp_copy_data(data, datafd, opts, progress_label, expected);
	/* Make transfer completion visible to servers immediately.  Some servers do
	 * not send 226 until the client has closed its side of the data connection.
	 */
	shutdown(data, SHUT_RDWR);
	close(data);

	code = ftp_read_reply(ctrl, ctrlfd, 0, echo, NULL, 0);
	if (code != 226 && code != 250) {
		fprintf(stderr, "nfetch: ftp: transfer completion failed, reply %d\n", code);
		return 1;
	}

	return status;
}

static int ftp_fetch_path(int ctrl, const nfetch_opts_t *opts,
					  const nfetch_url_t *url, int outfd)
{
	char cmd[4096];
	const char *path = url->path && url->path[0] ? url->path : "/";

	if (strcmp(path, "/") == 0) {
		snprintf(cmd, sizeof(cmd), "LIST");
		return ftp_transfer(ctrl, opts, url->host, outfd, STDERR_FILENO, cmd, "A", opts->verbose, NULL);
	}

	if (path[strlen(path) - 1] == '/') {
		if (snprintf(cmd, sizeof(cmd), "LIST %s",
				 path[0] == '/' ? path + 1 : path) >= (int)sizeof(cmd)) {
			fprintf(stderr, "nfetch: ftp: path too large\n");
			return 1;
		}

		return ftp_transfer(ctrl, opts, url->host, outfd, STDERR_FILENO, cmd, "A", opts->verbose, NULL);
	}

	if (snprintf(cmd, sizeof(cmd), "RETR %s", path[0] == '/' ? path + 1 : path) >=
		(int)sizeof(cmd)) {
		fprintf(stderr, "nfetch: ftp: path too large\n");
		return 1;
	}

	return ftp_transfer(ctrl, opts, url->host, outfd, STDERR_FILENO, cmd, "I", opts->verbose, url->path);
}

static int ftp_send_simple(int ctrl, const nfetch_opts_t *opts, int outfd,
					   const char *cmd, int echo);

static int ftp_repl_help(int ctrl, const nfetch_opts_t *opts, int echo)
{
	(void)opts;
	fprintf(stderr, "nfetch: ftp: server HELP\n");
	if (ftp_send_simple(ctrl, opts, STDERR_FILENO, "HELP", echo) != 0)
		return 1;

	fprintf(stderr, "nfetch: ftp: server FEAT\n");
	return ftp_send_simple(ctrl, opts, STDERR_FILENO, "FEAT", echo);
}

static char *ftp_trim(char *s)
{
	char *e;

	while (*s && isspace((unsigned char)*s))
		s++;

	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';

	return s;
}

static void ftp_split_word(char *line, char **word, char **args)
{
	*word = line;
	while (*line && !isspace((unsigned char)*line))
		line++;
	if (*line)
		*line++ = '\0';
	*args = ftp_trim(line);
}

static int ftp_streq_ci(const char *a, const char *b)
{
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

static void ftp_upper_cmd_word(char *s)
{
	while (*s && !isspace((unsigned char)*s)) {
		*s = (char)toupper((unsigned char)*s);
		s++;
	}
}

static int ftp_repl_get(const nfetch_opts_t *opts, const nfetch_url_t *url, int ctrl,
						char *args, int outfd, int echo)
{
	char *path;
	char *file;
	int fd;
	char cmd[4096];
	int status;

	args = ftp_trim(args);

	if (!*args) {
		fprintf(stderr, "nfetch: ftp: get requires PATH\n");
		return 0;
	}

	path = args;
	while (*args && !isspace((unsigned char)*args))
		args++;

	if (*args)
		*args++ = '\0';

	file = ftp_trim(args);
	fd = *file ? nfetch_open_output(file) : outfd;
	if (fd < 0)
		return 1;

	if (snprintf(cmd, sizeof(cmd), "RETR %s", path) >= (int)sizeof(cmd)) {
		fprintf(stderr, "nfetch: ftp: path too large\n");
		if (fd != outfd && fd != STDOUT_FILENO)
			close(fd);
		return 1;
	}

	status = ftp_transfer(ctrl, opts, url->host, fd, STDERR_FILENO, cmd, "I", echo, path);
	if (fd != outfd && fd != STDOUT_FILENO)
		close(fd);
	return status;
}

static int ftp_repl_list(const nfetch_opts_t *opts, const nfetch_url_t *url, int ctrl,
						 const char *verb, char *args, int outfd, int echo)
{
	char cmd[4096];

	args = ftp_trim(args);

	if (*args) {
		if (snprintf(cmd, sizeof(cmd), "%s %s", verb, args) >= (int)sizeof(cmd)) {
			fprintf(stderr, "nfetch: ftp: path too large\n");
			return 1;
		}
	} else {
		snprintf(cmd, sizeof(cmd), "%s", verb);
	}

	return ftp_transfer(ctrl, opts, url->host, outfd, STDERR_FILENO, cmd, "A", echo, NULL);
}

static int ftp_send_simple(int ctrl, const nfetch_opts_t *opts, int outfd,
					   const char *cmd, int echo)
{
	int code;
	(void)opts;

	if (ftp_write_cmd(ctrl, cmd) < 0) {
		fprintf(stderr, "nfetch: ftp: write: %s\n", strerror(errno));
		return 1;
	}

	code = ftp_read_reply(ctrl, outfd, 0, echo, NULL, 0);
	if (code < 0)
		return 1;

	return 0;
}

static int ftp_repl(const nfetch_opts_t *opts, const nfetch_url_t *url, int ctrl, int outfd)
{
	int echo = 1;

	fprintf(stderr, "nfetch: ftp interactive mode; help asks the server for HELP/FEAT\n");

	for (;;) {
		fputs("ftp> ", stderr);
		fflush(stderr);

		char *raw = sh_read_line(stdin);
		char *line;
		char *cmd;
		char *args;
		int slash = 0;
		int r = 0;

		if (!raw)
			break;

		line = ftp_trim(raw);
		if (!*line) {
			free(raw);
			continue;
		}

		if (*line == '/') {
			slash = 1;
			line = ftp_trim(line + 1);
			if (!*line) {
				free(raw);
				continue;
			}
		}

		ftp_split_word(line, &cmd, &args);

		if (ftp_streq_ci(cmd, "help") || strcmp(cmd, "?") == 0) {
			r = ftp_repl_help(ctrl, opts, echo);
			free(raw);
			if (r)
				return r;
			continue;
		}

		if (ftp_streq_ci(cmd, "quit") || ftp_streq_ci(cmd, "bye") ||
			ftp_streq_ci(cmd, "exit")) {
			ftp_send_simple(ctrl, opts, STDERR_FILENO, "QUIT", echo);
			free(raw);
			break;
		}

		if (ftp_streq_ci(cmd, "pwd")) {
			r = ftp_send_simple(ctrl, opts, STDERR_FILENO, "PWD", echo);
		} else if (ftp_streq_ci(cmd, "cd") || ftp_streq_ci(cmd, "cwd")) {
			char buf[4096];
			if (!*args) {
				fprintf(stderr, "nfetch: ftp: cd requires PATH\n");
				free(raw);
				continue;
			}
			if (snprintf(buf, sizeof(buf), "CWD %s", args) >= (int)sizeof(buf)) {
				fprintf(stderr, "nfetch: ftp: path too large\n");
				free(raw);
				continue;
			}
			r = ftp_send_simple(ctrl, opts, STDERR_FILENO, buf, echo);
		} else if (ftp_streq_ci(cmd, "cdup")) {
			r = ftp_send_simple(ctrl, opts, STDERR_FILENO, "CDUP", echo);
		} else if (ftp_streq_ci(cmd, "ls") || ftp_streq_ci(cmd, "dir") ||
				   ftp_streq_ci(cmd, "list")) {
			r = ftp_repl_list(opts, url, ctrl, "LIST", args, outfd, echo);
		} else if (ftp_streq_ci(cmd, "nlst")) {
			r = ftp_repl_list(opts, url, ctrl, "NLST", args, outfd, echo);
		} else if (ftp_streq_ci(cmd, "get") || ftp_streq_ci(cmd, "retr")) {
			r = ftp_repl_get(opts, url, ctrl, args, outfd, echo);
		} else if (ftp_streq_ci(cmd, "ascii")) {
			r = ftp_send_simple(ctrl, opts, STDERR_FILENO, "TYPE A", echo);
		} else if (ftp_streq_ci(cmd, "binary") || ftp_streq_ci(cmd, "bin")) {
			r = ftp_send_simple(ctrl, opts, STDERR_FILENO, "TYPE I", echo);
		} else if (ftp_streq_ci(cmd, "quote") || ftp_streq_ci(cmd, "literal")) {
			if (!*args) {
				fprintf(stderr, "nfetch: ftp: quote requires COMMAND\n");
				free(raw);
				continue;
			}
			r = ftp_send_simple(ctrl, opts, STDERR_FILENO, args, echo);
		} else if (slash) {
			fprintf(stderr, "nfetch: ftp: unknown client command: /%s\n", cmd);
			fprintf(stderr, "nfetch: ftp: use quote %s to send it literally\n", cmd);
		} else {
			/* Raw plain FTP command. Uppercase only the command verb. */
			char *full = cmd;
			if (*args) {
				cmd[strlen(cmd)] = ' ';
			}
			ftp_upper_cmd_word(full);
			r = ftp_send_simple(ctrl, opts, STDERR_FILENO, full, echo);
		}

		free(raw);
		if (r)
			return r;
	}

	return 0;
}

int nfetch_scheme_ftp(const nfetch_opts_t *opts, const nfetch_url_t *url)
{
	int interactive = !opts->send_data && (!url->path || strcmp(url->path, "/") == 0);
	int outfd = nfetch_open_output(opts->outfile);
	int ctrl;
	int status = 0;

	if (outfd < 0)
		return 1;

	ctrl = nfetch_connect_tcp_timeout(url->host, url->port, opts->verbose, opts->timeout_sec);
	if (ctrl < 0) {
		if (outfd != STDOUT_FILENO)
			close(outfd);
		return 1;
	}

	if (ftp_login(ctrl, opts, STDERR_FILENO, interactive || opts->verbose) != 0) {
		close(ctrl);
		if (outfd != STDOUT_FILENO)
			close(outfd);
		return 1;
	}

	if (opts->send_data) {
		size_t send_len = 0;
		char *send_buf = nfetch_decode_escapes(opts->send_data, &send_len);

		if (send_len > 0 && nfetch_sock_write_all(ctrl, send_buf, send_len) < 0) {
			fprintf(stderr, "nfetch: ftp: write: %s\n", strerror(errno));
			status = 1;
		} else if (send_len == 0 || send_buf[send_len - 1] != '\n') {
			if (nfetch_sock_write_all(ctrl, "\r\n", 2) < 0) {
				fprintf(stderr, "nfetch: ftp: write: %s\n", strerror(errno));
				status = 1;
			}
		}

		free(send_buf);
		if (!status)
			status = ftp_read_reply(ctrl, STDERR_FILENO, 0, 1, NULL, 0) < 0;
	} else if (url->path && strcmp(url->path, "/") != 0) {
		status = ftp_fetch_path(ctrl, opts, url, outfd);
	} else {
		status = ftp_repl(opts, url, ctrl, outfd);
	}

	if (!interactive)
		ftp_write_cmd(ctrl, "QUIT");
	close(ctrl);

	if (outfd != STDOUT_FILENO)
		close(outfd);

	return status;
}
