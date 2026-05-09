#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <sh.h>
#include "scheme.h"

#ifndef SO_RCVTIMEO
#define SO_RCVTIMEO 20
#endif

void nfetch_url_free(nfetch_url_t *u)
{
	if (!u)
		return;

	free(u->scheme);
	free(u->host);
	free(u->port);
	free(u->path);
	memset(u, 0, sizeof(*u));
}

void nfetch_set_recv_timeout(int s, int timeout_sec)
{
	struct timeval tv;

	if (timeout_sec <= 0) {
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	} else {
		tv.tv_sec = timeout_sec;
		tv.tv_usec = 0;
	}

	(void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int nfetch_write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len > 0) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		if (n == 0) {
			errno = EPIPE;
			return -1;
		}

		p += n;
		len -= (size_t)n;
	}

	return 0;
}

int nfetch_sock_write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len > 0) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		if (n == 0) {
			errno = EPIPE;
			return -1;
		}

		p += n;
		len -= (size_t)n;
	}

	return 0;
}

int nfetch_open_output(const char *path)
{
	if (!path)
		return STDOUT_FILENO;

	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);

	if (fd < 0)
		fprintf(stderr, "nfetch: %s: %s\n", path, strerror(errno));

	return fd;
}

int nfetch_copy_response(int s, int outfd, int timeout_sec)
{
	char buf[8192];
	unsigned long total = 0;

	nfetch_set_recv_timeout(s, timeout_sec);

	for (;;) {
		ssize_t n = recv(s, buf, sizeof(buf), 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;

			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
				if (total == 0) {
					fprintf(stderr, "nfetch: timeout: received 0 bytes\n");
					return 1;
				}

				break;
			}

			fprintf(stderr, "nfetch: recv: %s\n", strerror(errno));
			return 1;
		}

		if (n == 0)
			break;

		if (nfetch_write_all(outfd, buf, (size_t)n) < 0) {
			fprintf(stderr, "nfetch: write: %s\n", strerror(errno));
			return 1;
		}

		total += (unsigned long)n;
	}

	return 0;
}

static int nfetch_http_header_done_state(int state, unsigned char c)
{
	/*
	 * Return -1 once the HTTP header terminator has been consumed.
	 * States recognize both CRLFCRLF and bare LFLF without buffering the
	 * whole header block. This matters on lyr, where builtin stack space is
	 * small enough that a 64 KiB automatic header buffer can page fault.
	 */
	switch (state) {
	case 0:
		if (c == '\r')
			return 1;
		if (c == '\n')
			return 3;
		return 0;
	case 1: /* CR */
		if (c == '\n')
			return 2;
		if (c == '\r')
			return 1;
		return 0;
	case 2: /* CRLF */
		if (c == '\r')
			return 4;
		if (c == '\n')
			return -1;
		return 0;
	case 3: /* LF */
		if (c == '\n')
			return -1;
		if (c == '\r')
			return 1;
		return 0;
	case 4: /* CRLFCR */
		if (c == '\n')
			return -1;
		if (c == '\r')
			return 1;
		return 0;
	default:
		return 0;
	}
}

int nfetch_copy_http_body(int s, int outfd, int timeout_sec)
{
	char buf[8192];
	unsigned long total = 0;
	unsigned long header_total = 0;
	int in_headers = 1;
	int header_state = 0;

	nfetch_set_recv_timeout(s, timeout_sec);

	for (;;) {
		ssize_t n = recv(s, buf, sizeof(buf), 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;

			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
				if (total == 0) {
					fprintf(stderr, "nfetch: timeout: received 0 bytes\n");
					return 1;
				}

				break;
			}

			fprintf(stderr, "nfetch: recv: %s\n", strerror(errno));
			return 1;
		}

		if (n == 0)
			break;

		total += (unsigned long)n;

		if (in_headers) {
			size_t body_off = 0;
			int found = 0;

			for (size_t i = 0; i < (size_t)n; i++) {
				header_total++;
				if (header_total > 65536) {
					fprintf(stderr, "nfetch: HTTP headers too large\n");
					return 1;
				}

				header_state = nfetch_http_header_done_state(
					header_state, (unsigned char)buf[i]);
				if (header_state < 0) {
					body_off = i + 1;
					found = 1;
					break;
				}
			}

			if (!found)
				continue;

			in_headers = 0;

			if (body_off < (size_t)n &&
				nfetch_write_all(outfd, buf + body_off,
							 (size_t)n - body_off) < 0) {
				fprintf(stderr, "nfetch: write: %s\n", strerror(errno));
				return 1;
			}

			continue;
		}

		if (nfetch_write_all(outfd, buf, (size_t)n) < 0) {
			fprintf(stderr, "nfetch: write: %s\n", strerror(errno));
			return 1;
		}
	}

	return 0;
}

int nfetch_drain_response_once(int s, int outfd, int timeout_sec)
{
	char buf[8192];

	nfetch_set_recv_timeout(s, timeout_sec);

	for (;;) {
		ssize_t n = recv(s, buf, sizeof(buf), 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;

			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)
				return 0;

			fprintf(stderr, "nfetch: recv: %s\n", strerror(errno));
			return 1;
		}

		if (n == 0) {
			fprintf(stderr, "nfetch: peer closed connection\n");
			return 2;
		}

		if (nfetch_write_all(outfd, buf, (size_t)n) < 0) {
			fprintf(stderr, "nfetch: write: %s\n", strerror(errno));
			return 1;
		}

		if (n < (ssize_t)sizeof(buf))
			return 0;
	}
}

int nfetch_parse_url(const char *input, nfetch_url_t *out)
{
	memset(out, 0, sizeof(*out));

	const char *p = input;
	const char *scheme_end = strstr(input, "://");

	if (scheme_end) {
		out->scheme = sh_strndup(input, (size_t)(scheme_end - input));
		p = scheme_end + 3;
	} else {
		out->scheme = sh_xstrdup("http");
		p = input;
	}

	if (strcmp(out->scheme, "http") != 0 && strcmp(out->scheme, "tcp") != 0 &&
		strcmp(out->scheme, "ftp") != 0) {
		fprintf(stderr, "nfetch: unsupported scheme: %s\n", out->scheme);
		return -1;
	}

	if (!*p) {
		fprintf(stderr, "nfetch: missing host\n");
		return -1;
	}

	const char *host_start = p;
	const char *host_end = p;

	while (*host_end && *host_end != ':' && *host_end != '/')
		host_end++;

	if (host_end == host_start) {
		fprintf(stderr, "nfetch: missing host\n");
		return -1;
	}

	out->host = sh_strndup(host_start, (size_t)(host_end - host_start));

	if (*host_end == ':') {
		const char *port_start = host_end + 1;
		const char *port_end = port_start;

		while (*port_end && *port_end != '/')
			port_end++;

		if (port_end == port_start) {
			fprintf(stderr, "nfetch: missing port after ':'\n");
			return -1;
		}

		out->port = sh_strndup(port_start, (size_t)(port_end - port_start));
		host_end = port_end;
	} else {
		if (strcmp(out->scheme, "ftp") == 0)
			out->port = sh_xstrdup("21");
		else
			out->port = sh_xstrdup("80");
	}

	if (*host_end == '/')
		out->path = sh_xstrdup(host_end);
	else
		out->path = sh_xstrdup("/");

	return 0;
}

static int nfetch_connect_addr_timeout(int s, const struct sockaddr *addr,
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

int nfetch_connect_tcp_timeout(const char *host, const char *port, int verbose,
					   int timeout_sec)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *rp;
	int s = -1;
	int last_errno = 0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	int r = getaddrinfo(host, port, &hints, &res);

	if (r != 0) {
		fprintf(stderr, "nfetch: resolve %s:%s: %s\n", host, port,
				gai_strerror(r));
		return -1;
	}

	for (rp = res; rp; rp = rp->ai_next) {
		s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

		if (s < 0) {
			last_errno = errno;
			continue;
		}

		if (nfetch_connect_addr_timeout(s, rp->ai_addr, rp->ai_addrlen,
								timeout_sec) == 0)
			break;

		last_errno = errno;
		close(s);
		s = -1;
	}

	freeaddrinfo(res);

	if (s < 0) {
		errno = last_errno ? last_errno : errno;
		fprintf(stderr, "nfetch: connect %s:%s: %s\n", host, port,
				strerror(errno));
		return -1;
	}

	if (verbose)
		fprintf(stderr, "nfetch: connected to %s:%s\n", host, port);

	return s;
}

int nfetch_connect_tcp(const char *host, const char *port, int verbose)
{
	return nfetch_connect_tcp_timeout(host, port, verbose, 5);
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';

	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;

	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return -1;
}

char *nfetch_decode_escapes(const char *s, size_t *len_out)
{
	size_t in_len = strlen(s);
	char *out = sh_xmalloc(in_len + 1);
	size_t o = 0;

	for (size_t i = 0; i < in_len; i++) {
		if (s[i] != '\\') {
			out[o++] = s[i];
			continue;
		}

		i++;

		if (!s[i]) {
			out[o++] = '\\';
			break;
		}

		switch (s[i]) {
		case 'n':
			out[o++] = '\n';
			break;
		case 'r':
			out[o++] = '\r';
			break;
		case 't':
			out[o++] = '\t';
			break;
		case '0':
			out[o++] = '\0';
			break;
		case '\\':
			out[o++] = '\\';
			break;
		case 'x': {
			int h1 = -1;
			int h2 = -1;

			if (i + 1 < in_len)
				h1 = hexval((unsigned char)s[i + 1]);

			if (i + 2 < in_len)
				h2 = hexval((unsigned char)s[i + 2]);

			if (h1 >= 0 && h2 >= 0) {
				out[o++] = (char)((h1 << 4) | h2);
				i += 2;
			} else {
				out[o++] = 'x';
			}

			break;
		}
		default:
			out[o++] = s[i];
			break;
		}
	}

	out[o] = '\0';

	if (len_out)
		*len_out = o;

	return out;
}