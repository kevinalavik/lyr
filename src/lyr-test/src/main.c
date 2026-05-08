#include <echo_test.h>
#include <http.h>
#include <netutil.h>
#include <nist_time.h>
#include <ping.h>
#include <web_server.h>

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static void route_index(const struct http_request *req,
						struct http_response *res, void *ctx)
{
	(void)req;
	(void)ctx;
	http_response_text(res, 200, "OK", "Hello from lyrOS userspace!\n");
}

static void route_alive(const struct http_request *req,
						struct http_response *res, void *ctx)
{
	(void)req;
	(void)ctx;
	http_response_text(res, 200, "OK", "alive\n");
}

static int check_local_service(void)
{
	struct in_addr ip;
	char ipstr[INET_ADDRSTRLEN];

	if (resolve_ipv4("lyr.local", &ip) < 0) {
		printf("\033[1;31mlyr.local:\033[0m resolve failed, continuing\n");
		return -1;
	}

	if (inet_ntop(AF_INET, &ip, ipstr, sizeof(ipstr)) == NULL) {
		print_errno("lyr.local inet_ntop");
		return -1;
	}

	printf("\033[1;36mlyr.local -> %s\033[0m\n", ipstr);

	if (strcmp(ipstr, "127.0.0.1") != 0)
		return 0;

	printf(
		"\033[1;34mlyr.local:\033[0m resolved to loopback, checking port 6969...\n");

	if (http_get_ip_port(ip, 6969, "lyr.local", "/alive") < 0) {
		printf(
			"\033[1;31mlyr.local:\033[0m port 6969 is not reachable or HTTP GET failed, continuing\n");
		return -1;
	}

	printf("\n\033[1;32mlyr.local:\033[0m HTTP GET on 127.0.0.1:6969 ok\n");
	return 0;
}

int main(void)
{
	static const struct http_route routes[] = {
		{ "GET", "/", route_index, NULL },
		{ "GET", "/alive", route_alive, NULL },
	};

	struct nist_time nt;
	int failures = 0;

	printf("\033[1;36mHello, World from mlibc!\033[0m\n");

	if (tcpbin_echo_test() < 0) {
		printf("\033[1;31mecho:\033[0m failed, continuing\n");
		failures++;
	}

	if (ping("8.8.8.8", PING_DEFAULT_COUNT) < 0)
		failures++;

	ping("localhost", 2);
	ping("0.0.0.0", 2);

	if (get_nist_time(&nt) < 0) {
		printf("\033[1;31mtime:\033[0m failed, continuing\n");
		failures++;
	} else {
		print_nist_time(&nt);
	}

	if (check_local_service() < 0)
		failures++;

	/* Runs forever on success. */
	if (http_server_listen(80, routes, sizeof(routes) / sizeof(routes[0])) <
		0) {
		printf("\033[1;31mweb:\033[0m failed\n");
		failures++;
	}

	if (failures > 0) {
		printf("\033[1;31mtests:\033[0m completed with %d failure(s)\n",
			   failures);
		return 1;
	}

	printf("\033[1;32mtests:\033[0m all tests passed\n");
	return 0;
}