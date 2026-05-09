#include <echo_test.h>
#include <http.h>
#include <netutil.h>
#include <nist_time.h>
#include <ping.h>
#include <web_server.h>

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <lyr/pci.h>
#include <pwd.h>
#include <sys/select.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

extern char **environ;

static int read_exact(FILE *f, void *buf, size_t len)
{
	unsigned char *p = buf;
	size_t done = 0;

	while (done < len) {
		size_t n = fread(p + done, 1, len - done, f);

		if (n == 0) {
			if (ferror(f))
				return -1;
			break;
		}

		done += n;
	}

	return done == len ? 1 : 0;
}

int test_pcimap_user_info(void)
{
	FILE *f = fopen(LYR_PCI_MAP_PATH, "rb");
	if (!f) {
		fprintf(stderr, "failed to open %s: %s\n", LYR_PCI_MAP_PATH,
				strerror(errno));
		return 1;
	}

	printf("%-8s %-9s %-8s %-7s %-5s %-32s %-32s\n", "PCI-ADDR", "ID", "CLASS",
		   "PROG-IF", "BOUND", "DEVICE", "DRIVER");

	printf("%-8s %-9s %-8s %-7s %-5s %-32s %-32s\n", "--------", "---------",
		   "--------", "-------", "-----", "--------------------------------",
		   "--------------------------------");

	for (;;) {
		lyr_pcimap_t ent;
		int r = read_exact(f, &ent, sizeof(ent));

		if (r < 0) {
			fprintf(stderr, "failed to read %s: %s\n", LYR_PCI_MAP_PATH,
					strerror(errno));
			fclose(f);
			return 1;
		}

		if (r == 0)
			break;

		if (ent.abi_version != LYR_PCI_MAP_ABI_VERSION) {
			fprintf(stderr, "unsupported PCI map ABI version %u, expected %u\n",
					ent.abi_version, LYR_PCI_MAP_ABI_VERSION);
			fclose(f);
			return 1;
		}

		if (ent.entry_size != sizeof(ent)) {
			fprintf(stderr, "unsupported PCI map entry size %u, expected %zu\n",
					ent.entry_size, sizeof(ent));
			fclose(f);
			return 1;
		}

		printf("%02x:%02x.%u  "
			   "%04x:%04x "
			   "%02x:%02x    "
			   "%02x      "
			   "%-5s "
			   "%-32.*s "
			   "%-32.*s\n",
			   ent.bus, ent.slot, ent.function, ent.vendor_id, ent.device_id,
			   ent.class_code, ent.subclass, ent.prog_if,
			   ent.bound ? "yes" : "no", (int)sizeof(ent.device_name),
			   ent.device_name, (int)sizeof(ent.driver_name), ent.driver_name);
	}

	fclose(f);

	printf("\nENVIRONMENT\n");

	if (!environ) {
		printf("(no environ)\n");
	} else {
		for (char **env = environ; *env; env++)
			printf("%s\n", *env);
	}

	{
		char cwd[PATH_MAX];

		errno = 0;
		if (!getcwd(cwd, sizeof(cwd))) {
			fprintf(stderr, "\ngetcwd failed: %s\n", strerror(errno));
		} else {
			printf("\nCURRENT DIRECTORY\n");
			printf("%s\n", cwd);
		}
	}

	{
		uid_t uid = getuid();
		uid_t euid = geteuid();
		gid_t gid = getgid();
		gid_t egid = getegid();
		struct passwd *pw;

		printf("\nCURRENT USER\n");
		printf("uid:   %lu\n", (unsigned long)uid);
		printf("euid:  %lu\n", (unsigned long)euid);
		printf("gid:   %lu\n", (unsigned long)gid);
		printf("egid:  %lu\n", (unsigned long)egid);

		errno = 0;
		pw = getpwuid(uid);

		if (!pw) {
			if (errno) {
				fprintf(stderr, "getpwuid(%lu) failed: %s\n",
						(unsigned long)uid, strerror(errno));
			} else {
				fprintf(stderr, "getpwuid(%lu): user not found\n",
						(unsigned long)uid);
			}
		} else {
			printf("name:  %s\n", pw->pw_name);
			printf("home:  %s\n", pw->pw_dir);
			printf("shell: %s\n", pw->pw_shell);
		}
	}

	printf("\nKEYBOARD POLL TEST (press keys to see them):\n");

	FILE *kbd = fopen("/dev/input/kbd", "rb");
	if (!kbd) {
		fprintf(stderr, "failed to open /dev/input/kbd: %s\n", strerror(errno));
		return 1;
	}

	while (1) {
		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(fileno(kbd), &readfds);
		struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
		int r = select(fileno(kbd) + 1, &readfds, NULL, NULL, &timeout);
		if (r < 0) {
			fprintf(stderr, "select failed: %s\n", strerror(errno));
			break;
		}
		if (r == 0)
			continue;
		char buf[64];
		size_t n = fread(buf, 1, sizeof(buf) - 1, kbd);
		if (n > 0) {
			buf[n] = '\0';
			printf("read: ");
			for (size_t i = 0; i < n; i++) {
				if (buf[i] >= 32 && buf[i] < 127)
					putchar(buf[i]);
				else if (buf[i] == 13)
					printf("\\r");
				else if (buf[i] == 10)
					printf("\\n");
				else
					printf("[0x%02x]", (unsigned char)buf[i]);
			}
			printf("\n");
		}
	}

	fclose(kbd);
	return 0;
}

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

	test_pcimap_user_info();

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