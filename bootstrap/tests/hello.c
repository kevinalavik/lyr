#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(void)
{
	int s;
	struct sockaddr_in addr;
	const char msg[] = "hello from lyrOS/mlibc over TCP\n";
	char buf[256];
	ssize_t n;

	printf("\033[1;36mHello, World from mlibc!\033[0m\n\n");

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		printf("\033[1;31mtcp:\033[0m socket failed\n");
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(4242);
	addr.sin_addr.s_addr = inet_addr("45.79.112.203"); /* tcpbin.com */

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
	printf("\033[1;35mecho:\033[0m %s", buf);

	close(s);
	return 0;
}