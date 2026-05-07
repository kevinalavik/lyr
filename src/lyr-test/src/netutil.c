#include <netutil.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

void print_errno(const char *where)
{
	int e = errno;

	printf("\033[1;31m%s failed:\033[0m errno=%d (%s)\n", where, e,
		   strerror(e));
}

int write_all(int fd, const void *buf, size_t len, const char *where)
{
	const unsigned char *p = buf;
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, p + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;

			print_errno(where);
			return -1;
		}

		if (n == 0) {
			printf("\033[1;31m%s failed:\033[0m write returned 0\n", where);
			return -1;
		}

		off += (size_t)n;
	}

	return 0;
}

int read_some_text(int fd, char *buf, size_t len)
{
	size_t used = 0;

	if (len == 0)
		return -1;

	while (used + 1 < len) {
		ssize_t n = read(fd, buf + used, len - 1 - used);

		if (n < 0) {
			if (errno == EINTR)
				continue;

			/*
			 * Some public Daytime servers send the payload and then
			 * reset instead of closing cleanly with FIN. Accept data
			 * already received.
			 */
			if (errno == ECONNRESET && used > 0)
				break;

			return -1;
		}

		if (n == 0)
			break;

		used += (size_t)n;

		if (memchr(buf, '\n', used))
			break;
	}

	buf[used] = 0;

	return used > 0 ? (int)used : -1;
}

int resolve_ipv4(const char *host, struct in_addr *out)
{
	struct hostent *he;

	if (!host || !out)
		return -1;

	if (inet_pton(AF_INET, host, out) == 1)
		return 0;

	he = gethostbyname(host);
	if (!he)
		return -1;

	if (he->h_addrtype != AF_INET || he->h_length != sizeof(*out) ||
		he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
		return -1;
	}

	memcpy(out, he->h_addr_list[0], sizeof(*out));
	return 0;
}

int tcp_connect_ipv4(struct in_addr ip, unsigned short port)
{
	int s;
	struct sockaddr_in addr;

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr = ip;

	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(s);
		return -1;
	}

	return s;
}

int tcp_connect_host(const char *host, unsigned short port)
{
	struct in_addr ip;

	if (resolve_ipv4(host, &ip) < 0)
		return -1;

	return tcp_connect_ipv4(ip, port);
}