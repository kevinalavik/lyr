#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

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

	printf("\033[1;31m%s failed:\033[0m errno=%d (%s)\n", where, e,
		   strerror(e));
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
			printf("\033[1;31m%s failed:\033[0m write returned 0\n", where);
			return -1;
		}

		off += (size_t)n;
	}

	return 0;
}

static int http_get_ip_port(struct in_addr ip, unsigned short port,
							const char *host, const char *path)
{
	int s;
	struct sockaddr_in addr;
	char req[512];
	char buf[1024];
	ssize_t n;

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		print_errno("http socket");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr = ip;

	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		print_errno("http connect");
		close(s);
		return -1;
	}

	snprintf(req, sizeof(req),
			 "GET %s HTTP/1.0\r\n"
			 "Host: %s\r\n"
			 "Connection: close\r\n"
			 "\r\n",
			 path, host);

	if (write_all(s, req, strlen(req), "http write") < 0) {
		close(s);
		return -1;
	}

	printf("\033[1;34mhttp: GET http://%s:%u%s\033[0m\n", host, port, path);

	for (;;) {
		n = read(s, buf, sizeof(buf) - 1);

		if (n < 0) {
			if (errno == EINTR)
				continue;

			print_errno("http read");
			close(s);
			return -1;
		}

		if (n == 0)
			break;

		buf[n] = 0;
		printf("%s", buf);
	}

	close(s);
	return 0;
}

static int read_some_text(int fd, char *buf, size_t len)
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

static int tcp_connect_host(const char *host, unsigned short port)
{
	int s;
	struct sockaddr_in addr;
	struct hostent *he;
	struct in_addr ip;

	he = gethostbyname(host);
	if (!he)
		return -1;

	if (he->h_addrtype != AF_INET || he->h_length != sizeof(ip) ||
		he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
		return -1;
	}

	memcpy(&ip, he->h_addr_list[0], sizeof(ip));

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

static int tcpbin_echo_test(void)
{
	int s;
	const char msg[] =
		"lyrOS (c) 2026 Kevin Alavik, made for the love of computing and my beautiful girlfriend ♥\n";
	char buf[512];
	size_t got = 0;
	size_t want = strlen(msg);

	printf("\033[1;34mecho:\033[0m testing tcpbin.com:4242...\n");

	s = tcp_connect_host("tcpbin.com", 4242);
	if (s < 0) {
		printf("\033[1;31mecho:\033[0m failed to connect\n");
		return -1;
	}

	if (write_all(s, msg, want, "echo write") < 0) {
		close(s);
		return -1;
	}

	while (got < want && got + 1 < sizeof(buf)) {
		ssize_t n = read(s, buf + got, sizeof(buf) - 1 - got);

		if (n < 0) {
			if (errno == EINTR)
				continue;

			print_errno("echo read");
			close(s);
			return -1;
		}

		if (n == 0)
			break;

		got += (size_t)n;
	}

	close(s);

	buf[got] = 0;

	if (got != want || strcmp(buf, msg) != 0) {
		printf("\033[1;31mecho:\033[0m response mismatch\n");
		return -1;
	}

	printf("\033[1;32mecho:\033[0m ok, got back: %s", buf);
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

	if (sscanf(line, "%d %d-%d-%d %d:%d:%d %d %d %d %lf %31s %7s", &out->mjd,
			   &yy, &out->month, &out->day, &out->hour, &out->minute,
			   &out->second, &out->dst, &out->leap, &out->health, &out->ut1,
			   zone, sync) != 13) {
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

		s = tcp_connect_host("time.nist.gov", 13);
		if (s < 0)
			continue;

		n = read_some_text(s, buf, sizeof(buf));
		close(s);

		if (n < 0)
			continue;

		line = trim_leading_crlf(buf);
		if (*line == 0)
			continue;

		if (parse_nist_daytime(line, out) == 0)
			return 0;
	}

	return -1;
}

static void print_time_result(const struct nist_time *t)
{
	printf("\033[1;32mtime:\033[0m synced: %04d-%02d-%02d %02d:%02d:%02d %s\n",
		   t->year, t->month, t->day, t->hour, t->minute, t->second, t->zone);
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
	addr.sin_addr.s_addr = inet_addr("0.0.0.0");

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

	printf("\033[1;32mweb:\033[0m listening on 0.0.0.0:80\n");

	for (;;) {
		int c;
		char req[512];
		ssize_t n;

		c = accept(s, NULL, NULL);
		if (c < 0) {
			if (errno == EINTR)
				continue;

			print_errno("web accept");
			continue;
		}

		n = read(c, req, sizeof(req) - 1);
		if (n < 0) {
			print_errno("web read");
			close(c);
			continue;
		}

		if (n > 0) {
			req[n] = 0;
			printf("\033[1;34mweb:\033[0m request received\n");
		}

		if (write_all(c, resp, strlen(resp), "web write") == 0)
			printf("\033[1;35mweb:\033[0m served request\n");

		close(c);
	}

	close(s);
	return 0;
}

int main(void)
{
	struct nist_time nt;

	printf("\033[1;36mHello, World from mlibc!\033[0m\n");

	if (tcpbin_echo_test() < 0)
		return 1;

	if (get_time(&nt) < 0) {
		printf("\033[1;31mtime:\033[0m failed\n");
		return 1;
	}

	print_time_result(&nt);

	/* test local hostname, should resolve to local ip */
	{
		struct in_addr ip;
		struct hostent *he;
		char ipstr[INET_ADDRSTRLEN];

		he = gethostbyname("lyr.local");
		if (!he)
			return -1;

		if (he->h_addrtype != AF_INET || he->h_length != sizeof(ip) ||
			he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
			return -1;
		}

		memcpy(&ip, he->h_addr_list[0], sizeof(ip));

		if (inet_ntop(AF_INET, &ip, ipstr, sizeof(ipstr)) == NULL)
			return -1;

		printf("\033[1;36mlyr.local -> %s\033[0m\n", ipstr);

		if (strcmp(ipstr, "127.0.0.1") == 0) {
			printf(
				"\033[1;34mlyr.local:\033[0m resolved to loopback, checking port 6969...\n");

			if (http_get_ip_port(ip, 6969, "lyr.local", "/alive") < 0) {
				printf(
					"\033[1;31mlyr.local:\033[0m port 6969 is not reachable or HTTP GET failed\n");
				return -1;
			}

			printf(
				"\n\033[1;32mlyr.local:\033[0m HTTP GET on 127.0.0.1:6969 ok\n");
		}
	}

	if (web_server() < 0)
		return 1;

	return 0;
}