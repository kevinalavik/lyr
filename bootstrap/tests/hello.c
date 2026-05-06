#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

struct nist_time {
	int mjd;

	int year;
	int month;
	int day;

	int hour;
	int minute;
	int second;

	int dst;
	int leap;
	int health;

	double ut1;
	char zone[32];
	char sync;
};

static void print_errno(const char *where)
{
	int e = errno;

	printf("\033[1;31m%s:\033[0m errno=%d (%s)\n", where, e, strerror(e));
}

static int write_all(int fd, const void *buf, size_t len, const char *where)
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
			printf("\033[1;31m%s:\033[0m write returned 0\n", where);
			return -1;
		}

		off += (size_t)n;
	}

	return 0;
}

static int read_exact(int fd, void *buf, size_t len, const char *where)
{
	unsigned char *p = buf;
	size_t off = 0;

	while (off < len) {
		ssize_t n = read(fd, p + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;

			print_errno(where);
			return -1;
		}

		if (n == 0) {
			printf("\033[1;31m%s:\033[0m unexpected EOF\n", where);
			return -1;
		}

		off += (size_t)n;
	}

	return 0;
}

static int read_some_text(int fd, char *buf, size_t len, const char *where)
{
	size_t used = 0;

	if (len == 0)
		return -1;

	while (used + 1 < len) {
		ssize_t n = read(fd, buf + used, len - 1 - used);

		if (n < 0) {
			int e = errno;

			if (e == EINTR)
				continue;

			/*
			 * Some public Daytime servers send the payload and then
			 * reset instead of closing cleanly with FIN. If we already
			 * received data, accept it.
			 */
			if (e == ECONNRESET && used > 0)
				break;

			errno = e;
			print_errno(where);
			return -1;
		}

		if (n == 0)
			break;

		used += (size_t)n;

		/*
		 * Daytime is line-oriented. Stop after one line if present.
		 */
		if (memchr(buf, '\n', used))
			break;
	}

	buf[used] = 0;

	if (used == 0) {
		printf("\033[1;31m%s:\033[0m empty response\n", where);
		return -1;
	}

	return (int)used;
}

static int dns_resolve_a_tcp(const char *host, struct in_addr *out)
{
	int s = -1;
	struct sockaddr_in dns;
	unsigned char q[512], r[4096], lp[2];
	size_t qlen = 12, off;
	unsigned short msglen, ancount;
	const char *p;
	int i;

#define U16(b) ((unsigned short)(((b)[0] << 8) | (b)[1]))
#define PUT16(b, v)                         \
	do {                                    \
		(b)[0] = (unsigned char)((v) >> 8); \
		(b)[1] = (unsigned char)(v);        \
	} while (0)

	memset(q, 0, sizeof(q));

	/* DNS header */
	q[0] = 0x12;
	q[1] = 0x34; /* ID */
	q[2] = 0x01;
	q[3] = 0x00; /* recursion desired */
	q[4] = 0x00;
	q[5] = 0x01; /* one question */

	/* Encode host as DNS labels: example.com -> 7 example 3 com 0 */
	p = host;
	while (*p) {
		const char *dot = strchr(p, '.');
		size_t len = dot ? (size_t)(dot - p) : strlen(p);

		if (len == 0 || len > 63 || qlen + 1 + len >= sizeof(q)) {
			printf("\033[1;31mdns:\033[0m invalid hostname label in %s\n",
				   host);
			return -1;
		}

		q[qlen++] = (unsigned char)len;
		memcpy(q + qlen, p, len);
		qlen += len;

		if (!dot)
			break;

		p = dot + 1;
	}

	if (qlen + 5 > sizeof(q)) {
		printf("\033[1;31mdns:\033[0m query too large for %s\n", host);
		return -1;
	}

	q[qlen++] = 0; /* end of name */
	q[qlen++] = 0;
	q[qlen++] = 1; /* QTYPE A */
	q[qlen++] = 0;
	q[qlen++] = 1; /* QCLASS IN */

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		print_errno("dns socket");
		return -1;
	}

	memset(&dns, 0, sizeof(dns));
	dns.sin_family = AF_INET;
	dns.sin_port = htons(53);
	dns.sin_addr.s_addr = inet_addr("1.1.1.1");

	if (connect(s, (struct sockaddr *)&dns, sizeof(dns)) < 0) {
		print_errno("dns connect");
		goto fail;
	}

	PUT16(lp, qlen);

	if (write_all(s, lp, 2, "dns write length") < 0)
		goto fail;

	if (write_all(s, q, qlen, "dns write query") < 0)
		goto fail;

	if (read_exact(s, lp, 2, "dns read length") < 0)
		goto fail;

	msglen = U16(lp);
	if (msglen < 12 || msglen > sizeof(r)) {
		printf("\033[1;31mdns:\033[0m bad response length: %u\n", msglen);
		goto fail;
	}

	if (read_exact(s, r, msglen, "dns read response") < 0)
		goto fail;

	close(s);
	s = -1;

	if (r[0] != 0x12 || r[1] != 0x34) {
		printf("\033[1;31mdns:\033[0m response ID mismatch\n");
		return -1;
	}

	if ((U16(r + 2) & 0x000f) != 0) {
		printf("\033[1;31mdns:\033[0m DNS error rcode=%u\n",
			   U16(r + 2) & 0x000f);
		return -1;
	}

	ancount = U16(r + 6);

	/* Skip question name. */
	off = 12;
	while (off < msglen && r[off]) {
		if ((r[off] & 0xc0) == 0xc0) {
			off += 2;
			break;
		}

		if (off + 1 + r[off] > msglen) {
			printf("\033[1;31mdns:\033[0m malformed question name\n");
			return -1;
		}

		off += 1 + r[off];
	}

	if (off >= msglen) {
		printf("\033[1;31mdns:\033[0m truncated question name\n");
		return -1;
	}

	if (r[off] == 0)
		off++;

	if (off + 4 > msglen) {
		printf("\033[1;31mdns:\033[0m truncated question trailer\n");
		return -1;
	}

	off += 4; /* QTYPE + QCLASS */

	for (i = 0; i < ancount; i++) {
		unsigned short type, class_, rdlen;

		if (off >= msglen) {
			printf("\033[1;31mdns:\033[0m truncated answer name\n");
			return -1;
		}

		if ((r[off] & 0xc0) == 0xc0) {
			off += 2;
		} else {
			while (off < msglen && r[off]) {
				if (off + 1 + r[off] > msglen) {
					printf("\033[1;31mdns:\033[0m malformed answer name\n");
					return -1;
				}

				off += 1 + r[off];
			}

			if (off >= msglen) {
				printf("\033[1;31mdns:\033[0m truncated answer name\n");
				return -1;
			}

			off++;
		}

		if (off + 10 > msglen) {
			printf("\033[1;31mdns:\033[0m truncated answer header\n");
			return -1;
		}

		type = U16(r + off);
		class_ = U16(r + off + 2);
		rdlen = U16(r + off + 8);
		off += 10;

		if (off + rdlen > msglen) {
			printf("\033[1;31mdns:\033[0m truncated answer rdata\n");
			return -1;
		}

		if (type == 1 && class_ == 1 && rdlen == 4) {
			memcpy(&out->s_addr, r + off, 4);
			return 0;
		}

		off += rdlen;
	}

	printf("\033[1;31mdns:\033[0m no A record found for %s\n", host);
	return -1;

fail:
	if (s >= 0)
		close(s);
	return -1;

#undef U16
#undef PUT16
}

static int tcp_connect_host(const char *host, unsigned short port)
{
	int s;
	struct sockaddr_in addr;
	struct in_addr ip;

	if (dns_resolve_a_tcp(host, &ip) < 0)
		return -1;

	printf("\033[1;32mdns:\033[0m %s -> %s\n", host, inet_ntoa(ip));

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		print_errno("socket");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr = ip;

	printf("\033[1;34mtcp:\033[0m connecting to %s:%u...\n", host,
		   (unsigned)port);

	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		print_errno("connect");
		close(s);
		return -1;
	}

	printf("\033[1;32mtcp:\033[0m connected\n");
	return s;
}

static int tcpbin_echo_test(void)
{
	int s;
	const char msg[] =
		"lyrOS (c) 2026 Kevin Alavik, made for the love of computing and my beautiful girlfriend ♥\n";
	char buf[512];
	size_t got = 0;
	size_t want = strlen(msg);

	s = tcp_connect_host("tcpbin.com", 4242);
	if (s < 0)
		return -1;

	printf("\033[1;33msend:\033[0m %s", msg);

	if (write_all(s, msg, want, "tcp write") < 0) {
		close(s);
		return -1;
	}

	while (got < want && got + 1 < sizeof(buf)) {
		ssize_t n = read(s, buf + got, sizeof(buf) - 1 - got);

		if (n < 0) {
			if (errno == EINTR)
				continue;

			print_errno("tcp read");
			close(s);
			return -1;
		}

		if (n == 0) {
			printf("\033[1;31mtcp:\033[0m remote closed before full echo\n");
			close(s);
			return -1;
		}

		got += (size_t)n;
	}

	buf[got] = 0;

	if (got != want || strcmp(buf, msg) != 0) {
		printf("\033[1;31mtcp:\033[0m response mismatch\n");
		printf("expected length: %d\n", (int)want);
		printf("got length:      %d\n", (int)got);
		printf("expected:\n%s\n", msg);
		printf("got:\n%s\n", buf);
		close(s);
		return -1;
	}

	printf("\033[1;35mecho:\033[0m %s", buf);

	close(s);
	return 0;
}

static char *trim_leading_crlf(char *s)
{
	while (*s == '\r' || *s == '\n')
		s++;

	return s;
}

static int parse_nist_daytime(const char *line, struct nist_time *out)
{
	int yy;
	char zone[sizeof(out->zone)];
	char sync[8];

	memset(out, 0, sizeof(*out));
	memset(zone, 0, sizeof(zone));
	memset(sync, 0, sizeof(sync));

	/*
	 * Example:
	 *
	 *   61166 26-05-06 12:06:29 50 0 0 777.9 UTC(NIST) *
	 */
	if (sscanf(line, "%d %d-%d-%d %d:%d:%d %d %d %d %lf %31s %7s", &out->mjd,
			   &yy, &out->month, &out->day, &out->hour, &out->minute,
			   &out->second, &out->dst, &out->leap, &out->health, &out->ut1,
			   zone, sync) != 13) {
		printf("\033[1;31mdaytime:\033[0m could not parse response: %s\n",
			   line);
		return -1;
	}

	out->year = 2000 + yy;
	strncpy(out->zone, zone, sizeof(out->zone) - 1);
	out->sync = sync[0];

	return 0;
}

static int get_time(struct nist_time *out)
{
	int attempt;

	for (attempt = 0; attempt < 5; attempt++) {
		int s;
		char buf[512];
		char *line;
		int n;

		printf("\033[1;90mtime:\033[0m attempt %d/5\n", attempt + 1);

		s = tcp_connect_host("time.nist.gov", 13);
		if (s < 0)
			continue;

		n = read_some_text(s, buf, sizeof(buf), "daytime read");
		close(s);

		if (n < 0)
			continue;

		line = trim_leading_crlf(buf);

		if (*line == 0) {
			printf("\033[1;33mdaytime:\033[0m empty line, retrying\n");
			continue;
		}

		if (parse_nist_daytime(line, out) == 0)
			return 0;
	}

	printf("\033[1;31mtime:\033[0m failed to get NIST daytime after retries\n");
	return -1;
}

static void print_nist_time(const struct nist_time *t)
{
	printf("\033[1;35mtime:\033[0m %04d-%02d-%02d %02d:%02d:%02d %s\n", t->year,
		   t->month, t->day, t->hour, t->minute, t->second, t->zone);

	printf(
		"\033[1;90mtime info:\033[0m mjd=%d dst=%d leap=%d health=%d ut1=%.1f sync=%c\n",
		t->mjd, t->dst, t->leap, t->health, t->ut1, t->sync);
}

static int web_server(void)
{
	int s;
	struct sockaddr_in addr;

	static const char body[] = "Hello from lyrOS userspace!\n";

	char resp[512];

	snprintf(resp, sizeof(resp),
			 "HTTP/1.0 200 OK\r\n"
			 "Server: lyrOS-userspace\r\n"
			 "Content-Type: text/plain\r\n"
			 "Content-Length: %d\r\n"
			 "Connection: close\r\n"
			 "\r\n"
			 "%s",
			 (int)strlen(body), body);

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		print_errno("web socket");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(80);
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");

	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		print_errno("web bind");
		close(s);
		return -1;
	}

	if (listen(s, 8) < 0) {
		print_errno("web listen");
		close(s);
		return -1;
	}

	printf("\033[1;32mweb:\033[0m listening on 127.0.0.1:80\n");

	for (;;) {
		int c;
		char req[512];
		ssize_t n;

		c = accept(s, NULL, NULL);
		if (c < 0) {
			if (errno == EINTR)
				continue;

			if (errno == EAGAIN || errno == EWOULDBLOCK ||
				errno == ENETUNREACH) {
				print_errno("web accept");
				continue;
			}

			print_errno("web accept");
			close(s);
			return -1;
		}

		printf("\033[1;34mweb:\033[0m client connected\n");

		n = read(c, req, sizeof(req) - 1);
		if (n < 0) {
			print_errno("web read");
			close(c);
			continue;
		}

		if (n > 0) {
			req[n] = 0;
			printf("\033[1;90mweb request:\033[0m\n%s\n", req);
		}

		if (write_all(c, resp, strlen(resp), "web write") < 0) {
			close(c);
			continue;
		}

		close(c);

		printf("\033[1;35mweb:\033[0m served one request\n");
	}

	close(s);
	return 0;
}

int main(void)
{
	struct nist_time nt;

	printf("\033[1;36mHello, World from mlibc!\033[0m\n\n");

	if (tcpbin_echo_test() < 0)
		return 1;

	printf("\n");

	if (get_time(&nt) < 0)
		return 1;

	print_nist_time(&nt);
	printf("\n");

	if (web_server() < 0)
		return 1;

	return 0;
}