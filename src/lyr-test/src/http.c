#include "http.h"
#include "netutil.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int http_get_ip_port(struct in_addr ip, unsigned short port, const char *host,
					 const char *path)
{
	int s;
	char req[512];
	char buf[1024];
	ssize_t n;

	s = tcp_connect_ipv4(ip, port);
	if (s < 0) {
		print_errno("http connect");
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