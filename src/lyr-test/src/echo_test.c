#include <echo_test.h>
#include <netutil.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int tcpbin_echo_test(void)
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
