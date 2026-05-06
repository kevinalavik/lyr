#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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

		if (len == 0 || len > 63 || qlen + 1 + len >= sizeof(q))
			return -1;

		q[qlen++] = (unsigned char)len;
		memcpy(q + qlen, p, len);
		qlen += len;

		if (!dot)
			break;

		p = dot + 1;
	}

	if (qlen + 5 > sizeof(q))
		return -1;

	q[qlen++] = 0; /* end of name */
	q[qlen++] = 0;
	q[qlen++] = 1; /* QTYPE A */
	q[qlen++] = 0;
	q[qlen++] = 1; /* QCLASS IN */

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return -1;

	memset(&dns, 0, sizeof(dns));
	dns.sin_family = AF_INET;
	dns.sin_port = htons(53);
	dns.sin_addr.s_addr = inet_addr("1.1.1.1");

	if (connect(s, (struct sockaddr *)&dns, sizeof(dns)) < 0)
		goto fail;

	/* DNS over TCP has a 2-byte big-endian length prefix. */
	PUT16(lp, qlen);

	for (off = 0; off < 2;) {
		ssize_t n = write(s, lp + off, 2 - off);
		if (n <= 0)
			goto fail;
		off += (size_t)n;
	}

	for (off = 0; off < qlen;) {
		ssize_t n = write(s, q + off, qlen - off);
		if (n <= 0)
			goto fail;
		off += (size_t)n;
	}

	for (off = 0; off < 2;) {
		ssize_t n = read(s, lp + off, 2 - off);
		if (n <= 0)
			goto fail;
		off += (size_t)n;
	}

	msglen = U16(lp);
	if (msglen < 12 || msglen > sizeof(r))
		goto fail;

	for (off = 0; off < msglen;) {
		ssize_t n = read(s, r + off, msglen - off);
		if (n <= 0)
			goto fail;
		off += (size_t)n;
	}

	close(s);
	s = -1;

	if (r[0] != 0x12 || r[1] != 0x34)
		return -1;

	if ((U16(r + 2) & 0x000f) != 0)
		return -1;

	ancount = U16(r + 6);

	/* Skip question name. */
	off = 12;
	while (off < msglen && r[off]) {
		if ((r[off] & 0xc0) == 0xc0) {
			off += 2;
			break;
		}
		off += 1 + r[off];
	}

	if (off >= msglen)
		return -1;

	if (r[off] == 0)
		off++;

	if (off + 4 > msglen)
		return -1;

	off += 4; /* QTYPE + QCLASS */

	for (i = 0; i < ancount; i++) {
		unsigned short type, class_, rdlen;

		/* Skip answer name. */
		if (off >= msglen)
			return -1;

		if ((r[off] & 0xc0) == 0xc0) {
			off += 2;
		} else {
			while (off < msglen && r[off])
				off += 1 + r[off];

			if (off >= msglen)
				return -1;

			off++;
		}

		if (off + 10 > msglen)
			return -1;

		type = U16(r + off);
		class_ = U16(r + off + 2);
		rdlen = U16(r + off + 8);
		off += 10;

		if (off + rdlen > msglen)
			return -1;

		if (type == 1 && class_ == 1 && rdlen == 4) {
			memcpy(&out->s_addr, r + off, 4);
			return 0;
		}

		off += rdlen;
	}

	return -1;

fail:
	if (s >= 0)
		close(s);
	return -1;

#undef U16
#undef PUT16
}

int main(void)
{
	int s;
	struct sockaddr_in addr;
	const char msg[] =
		"lyrOS (c) 2026 Kevin Alavik, made for the love of computing and my beautiful girlfriend ♥\n";
	char buf[512];
	ssize_t n;
	struct in_addr ip;

	printf("\033[1;36mHello, World from mlibc!\033[0m\n\n");

	if (dns_resolve_a_tcp("tcpbin.com", &ip) < 0) {
		printf("\033[1;31mdns:\033[0m tcpbin.com resolve failed\n");
		return 1;
	}

	printf("\033[1;32mdns:\033[0m tcpbin.com -> %s\n", inet_ntoa(ip));

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		printf("\033[1;31mtcp:\033[0m socket failed\n");
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(4242);
	addr.sin_addr = ip;

	printf("\033[1;34mtcp:\033[0m connecting to tcpbin.com:4242...\n");

	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		printf("\033[1;31mtcp:\033[0m connect failed\n");
		close(s);
		return 1;
	}

	printf("\033[1;32mtcp:\033[0m connected\n");
	printf("\033[1;33msend:\033[0m %s", msg);

	if (write(s, msg, strlen(msg)) < 0) {
		printf("\033[1;31mtcp:\033[0m write failed\n");
		close(s);
		return 1;
	}

	n = read(s, buf, sizeof(buf) - 1);
	if (n < 0) {
		printf("\033[1;31mtcp:\033[0m read failed\n");
		close(s);
		return 1;
	}

	buf[n] = 0;
	if (strcmp(buf, msg) != 0) {
		printf("\033[1;31mtcp:\033[0m response mismatch\n");
		printf("expected:\n%s\n", msg);
		printf("got:\n%s\n", buf);
		close(s);
		return 1;
	}
	printf("\033[1;35mecho:\033[0m %s", buf);

	close(s);

	return 0;
}