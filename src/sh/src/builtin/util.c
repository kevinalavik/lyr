#include <ctype.h>
#include <errno.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <sh.h>
#include <builtin.h>

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1
#endif

#ifndef ICMP_ECHO
#define ICMP_ECHO 8
#endif

#ifndef ICMP_ECHOREPLY
#define ICMP_ECHOREPLY 0
#endif

#ifndef SO_RCVTIMEO
#define SO_RCVTIMEO 20
#endif

#define PING_DATA_SIZE 56
#define PING_TIMEOUT_SEC 1
#define PING_RX_SPIN_LIMIT 32

static const char *ls_uid_name(uid_t uid, char *buf, size_t buf_len);

int sh_builtin_type(int argc, char **argv)
{
	int status = 0;

	if (argc < 2) {
		fprintf(stderr, "type: missing operand\n");
		return 2;
	}

	for (int i = 1; i < argc; i++) {
		if (sh_is_builtin_name(argv[i])) {
			printf("%s is a shell builtin\n", argv[i]);
			continue;
		}

		char *path = sh_find_in_path(argv[i]);

		if (path) {
			printf("%s is %s\n", argv[i], path);
			free(path);
			continue;
		}

		fprintf(stderr, "type: %s: not found\n", argv[i]);
		status = 1;
	}

	return status;
}

int sh_builtin_which(int argc, char **argv)
{
	int status = 0;

	if (argc < 2) {
		fprintf(stderr, "which: missing operand\n");
		return 2;
	}

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			puts("usage: which command...");
			return 0;
		}

		char *path = sh_find_in_path(argv[i]);

		if (path) {
			puts(path);
			free(path);
			continue;
		}

		status = 1;
	}

	return status;
}

static const char *ls_uid_name(uid_t uid, char *buf, size_t buf_len)
{
	FILE *fp = fopen("/etc/passwd", "r");

	if (!fp) {
		snprintf(buf, buf_len, "%lu", (unsigned long)uid);
		return buf;
	}

	char line[512];

	while (fgets(line, sizeof(line), fp)) {
		if (line[0] == '\0' || line[0] == '\n' || line[0] == '#') {
			continue;
		}

		char *fields[8];
		char *p = line;

		for (int i = 0; i < 8; i++) {
			fields[i] = p;

			if (i < 7) {
				char *colon = strchr(p, ':');
				if (!colon) {
					fields[0] = NULL;
					break;
				}

				*colon = '\0';
				p = colon + 1;
			}
		}

		if (!fields[0]) {
			continue;
		}

		char *end = NULL;
		unsigned long parsed_uid = strtoul(fields[2], &end, 10);

		if (end == fields[2] || *end != '\0') {
			continue;
		}

		if ((uid_t)parsed_uid == uid) {
			snprintf(buf, buf_len, "%s", fields[0]);
			fclose(fp);
			return buf;
		}
	}

	fclose(fp);

	snprintf(buf, buf_len, "%lu", (unsigned long)uid);
	return buf;
}

static const char *ls_gid_name(gid_t gid, char *buf, size_t buf_len)
{
	FILE *fp = fopen("/etc/group", "r");

	if (!fp) {
		snprintf(buf, buf_len, "%lu", (unsigned long)gid);
		return buf;
	}

	char line[512];

	while (fgets(line, sizeof(line), fp)) {
		if (line[0] == '\0' || line[0] == '\n' || line[0] == '#') {
			continue;
		}

		char *fields[4];
		char *p = line;

		for (int i = 0; i < 4; i++) {
			fields[i] = p;

			if (i < 3) {
				char *colon = strchr(p, ':');
				if (!colon) {
					fields[0] = NULL;
					break;
				}

				*colon = '\0';
				p = colon + 1;
			}
		}

		if (!fields[0]) {
			continue;
		}

		char *end = NULL;
		unsigned long parsed_gid = strtoul(fields[2], &end, 10);

		if (end == fields[2] || *end != '\0') {
			continue;
		}

		if ((gid_t)parsed_gid == gid) {
			snprintf(buf, buf_len, "%s", fields[0]);
			fclose(fp);
			return buf;
		}
	}

	fclose(fp);

	snprintf(buf, buf_len, "%lu", (unsigned long)gid);
	return buf;
}

static void uname_print_field(const char *s, int *first)
{
	if (!*first)
		putchar(' ');

	fputs(s, stdout);
	*first = 0;
}

int sh_builtin_uname(int argc, char **argv)
{
	int show_sysname = 0;
	int show_nodename = 0;
	int show_release = 0;
	int show_version = 0;
	int show_machine = 0;
	int show_os = 0;
	struct utsname uts;
	int first = 1;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			puts("usage: uname [-amnrvspio]");
			puts("");
			puts("Print system information.");
			puts("");
			puts("Options:");
			puts("  -a, --all                 print all available information");
			puts(
				"  -s, --kernel-name         print kernel/system name (default)");
			puts("  -n, --nodename            print network node hostname");
			puts("  -r, --kernel-release      print kernel release");
			puts("  -v, --kernel-version      print kernel version");
			puts("  -m, --machine             print machine hardware name");
			puts("  -o, --operating-system    print operating system");
			return 0;
		}

		if (strcmp(argv[i], "--all") == 0) {
			show_sysname = 1;
			show_nodename = 1;
			show_release = 1;
			show_version = 1;
			show_machine = 1;
			show_os = 1;
			continue;
		}

		if (strcmp(argv[i], "--kernel-name") == 0) {
			show_sysname = 1;
			continue;
		}
		if (strcmp(argv[i], "--nodename") == 0) {
			show_nodename = 1;
			continue;
		}
		if (strcmp(argv[i], "--kernel-release") == 0) {
			show_release = 1;
			continue;
		}
		if (strcmp(argv[i], "--kernel-version") == 0) {
			show_version = 1;
			continue;
		}
		if (strcmp(argv[i], "--machine") == 0) {
			show_machine = 1;
			continue;
		}

		if (strcmp(argv[i], "--operating-system") == 0) {
			show_os = 1;
			continue;
		}

		if (argv[i][0] != '-' || argv[i][1] == '\0') {
			fprintf(stderr, "uname: extra operand: %s\n", argv[i]);
			return 2;
		}

		for (size_t j = 1; argv[i][j]; j++) {
			switch (argv[i][j]) {
			case 'a':
				show_sysname = 1;
				show_nodename = 1;
				show_release = 1;
				show_version = 1;
				show_machine = 1;
				show_os = 1;
				break;
			case 's':
				show_sysname = 1;
				break;
			case 'n':
				show_nodename = 1;
				break;
			case 'r':
				show_release = 1;
				break;
			case 'v':
				show_version = 1;
				break;
			case 'm':
				show_machine = 1;
				break;
			case 'o':
				show_os = 1;
				break;
			default:
				fprintf(stderr, "uname: invalid option -- '%c'\n", argv[i][j]);
				fprintf(stderr, "usage: uname [-amnrvspio]\n");
				return 2;
			}
		}
	}

	if (!show_sysname && !show_nodename && !show_release && !show_version &&
		!show_machine && !show_os)
		show_sysname = 1;

	if (uname(&uts) < 0) {
		fprintf(stderr, "uname: %s\n", strerror(errno));
		return 1;
	}

	if (show_sysname)
		uname_print_field(uts.sysname, &first);
	if (show_nodename)
		uname_print_field(uts.nodename, &first);
	if (show_release)
		uname_print_field(uts.release, &first);
	if (show_version)
		uname_print_field(uts.version, &first);
	if (show_machine)
		uname_print_field(uts.machine, &first);
	if (show_os)
		uname_print_field(uts.sysname, &first);

	putchar('\n');
	return 0;
}

int sh_builtin_id(void)
{
	uid_t uid = getuid();
	uid_t euid = geteuid();
	gid_t gid = getgid();
	gid_t egid = getegid();

	char uid_buf[32];
	char euid_buf[32];
	char gid_buf[32];
	char egid_buf[32];

	const char *user = ls_uid_name(uid, uid_buf, sizeof(uid_buf));
	const char *euser = ls_uid_name(euid, euid_buf, sizeof(euid_buf));
	const char *group = ls_gid_name(gid, gid_buf, sizeof(gid_buf));
	const char *egroup = ls_gid_name(egid, egid_buf, sizeof(egid_buf));

	printf("uid=%lu(%s) gid=%lu(%s) euid=%lu(%s) egid=%lu(%s)\n",
		   (unsigned long)uid, user, (unsigned long)gid, group,
		   (unsigned long)euid, euser, (unsigned long)egid, egroup);

	return 0;
}

int sh_builtin_whoami(void)
{
	uid_t uid = geteuid();
	char buf[32];
	puts(ls_uid_name(uid, buf, sizeof(buf)));
	return 0;
}

int sh_builtin_hexdump(int argc, char **argv)
{
	int status = 0;
	int saw_file = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			puts("usage: hexdump [file...]");
			return 0;
		}

		saw_file = 1;

		FILE *fp;

		if (strcmp(argv[i], "-") == 0) {
			fp = stdin;
		} else {
			fp = fopen(argv[i], "rb");

			if (!fp) {
				fprintf(stderr, "hexdump: %s: %s\n", argv[i], strerror(errno));
				status = 1;
				continue;
			}
		}

		unsigned char buf[16];
		unsigned long off = 0;

		for (;;) {
			size_t n = fread(buf, 1, sizeof(buf), fp);

			if (n == 0) {
				if (ferror(fp)) {
					fprintf(stderr, "hexdump: %s: %s\n", argv[i],
							strerror(errno));
					clearerr(fp);
					status = 1;
				}
				break;
			}

			printf("%08lx  ", off);

			for (size_t j = 0; j < 16; j++) {
				if (j < n)
					printf("%02x ", buf[j]);
				else
					printf("   ");

				if (j == 7)
					putchar(' ');
			}

			printf(" |");

			for (size_t j = 0; j < n; j++) {
				unsigned char c = buf[j];
				putchar((c >= 32 && c < 127) ? c : '.');
			}

			printf("|\n");

			off += (unsigned long)n;
		}

		if (fp != stdin)
			fclose(fp);
	}

	if (!saw_file) {
		unsigned char buf[16];
		unsigned long off = 0;

		for (;;) {
			size_t n = fread(buf, 1, sizeof(buf), stdin);

			if (n == 0) {
				if (ferror(stdin)) {
					fprintf(stderr, "hexdump: stdin: %s\n", strerror(errno));
					clearerr(stdin);
					status = 1;
				}
				break;
			}

			printf("%08lx  ", off);

			for (size_t j = 0; j < 16; j++) {
				if (j < n)
					printf("%02x ", buf[j]);
				else
					printf("   ");

				if (j == 7)
					putchar(' ');
			}

			printf(" |");

			for (size_t j = 0; j < n; j++) {
				unsigned char c = buf[j];
				putchar((c >= 32 && c < 127) ? c : '.');
			}

			printf("|\n");

			off += (unsigned long)n;
		}
	}

	return status;
}

struct ping_packet {
	struct icmphdr hdr;
	unsigned char data[PING_DATA_SIZE];
};

static unsigned short ping_checksum(const void *buf, int len)
{
	const unsigned short *data = buf;
	unsigned int sum = 0;

	while (len > 1) {
		sum += *data++;
		len -= 2;
	}

	if (len == 1)
		sum += *(const unsigned char *)data;

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);

	return (unsigned short)~sum;
}

static long ping_time_diff_us(const struct timespec *start,
							  const struct timespec *end)
{
	int64_t sec = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
	long nsec = end->tv_nsec - start->tv_nsec;
	int64_t us = sec * 1000000LL + (int64_t)(nsec / 1000L);

	if (us < 0)
		return 0;
	if (us > 0x7fffffffLL)
		return 0x7fffffffL;

	return (long)us;
}

static int ping_parse_ipv4(const char *s, struct in_addr *out)
{
	unsigned long parts[4];
	const char *p = s;

	for (int i = 0; i < 4; i++) {
		if (!isdigit((unsigned char)*p))
			return -1;

		char *end;
		unsigned long v = strtoul(p, &end, 10);

		if (v > 255)
			return -1;

		parts[i] = v;

		if (i < 3) {
			if (*end != '.')
				return -1;
			p = end + 1;
		} else {
			if (*end)
				return -1;
		}
	}

	uint32_t addr = ((uint32_t)parts[0] << 24) | ((uint32_t)parts[1] << 16) |
					((uint32_t)parts[2] << 8) | (uint32_t)parts[3];

	out->s_addr = htonl(addr);
	return 0;
}

static int ping_send_packet(int s, const struct sockaddr_in *addr,
							unsigned short ident, int seq,
							struct timespec *start)
{
	struct ping_packet pkt;

	memset(&pkt, 0, sizeof(pkt));

	pkt.hdr.type = ICMP_ECHO;
	pkt.hdr.code = 0;
	pkt.hdr.un.echo.id = htons(ident);
	pkt.hdr.un.echo.sequence = htons((unsigned short)seq);

	for (size_t i = 0; i < sizeof(pkt.data); i++)
		pkt.data[i] = (unsigned char)i;

	pkt.hdr.checksum = 0;
	pkt.hdr.checksum = ping_checksum(&pkt, sizeof(pkt));

	clock_gettime(CLOCK_MONOTONIC, start);

	if (sendto(s, &pkt, sizeof(pkt), 0, (const struct sockaddr *)addr,
			   sizeof(*addr)) < 0) {
		fprintf(stderr, "ping: sendto: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int ping_parse_reply(char *buf, ssize_t n, struct icmphdr **icmp_out,
							int *ttl_out)
{
	struct icmphdr *icmp;
	int ttl = -1;

	if (n >= (ssize_t)(sizeof(struct iphdr) + sizeof(struct icmphdr))) {
		struct iphdr *ip_hdr = (struct iphdr *)buf;
		size_t ip_hdr_len = (size_t)ip_hdr->ihl * 4;

		if (ip_hdr_len < sizeof(struct iphdr))
			return -1;

		if (n < (ssize_t)(ip_hdr_len + sizeof(struct icmphdr)))
			return -1;

		ttl = ip_hdr->ttl;
		icmp = (struct icmphdr *)(buf + ip_hdr_len);
	} else if (n >= (ssize_t)sizeof(struct icmphdr)) {
		icmp = (struct icmphdr *)buf;
	} else {
		return -1;
	}

	*icmp_out = icmp;
	*ttl_out = ttl;
	return 0;
}

static int ping_recv_packet(int s, unsigned short ident, int seq,
							long *rtt_us_out, int *ttl_out,
							const struct timespec *start)
{
	char recv_buf[512];
	int spins = 0;

	while (spins++ < PING_RX_SPIN_LIMIT) {
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);
		struct timespec end;
		struct icmphdr *icmp = NULL;
		int ttl = -1;

		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(s, &rfds);

		struct timespec ts;
		ts.tv_sec = PING_TIMEOUT_SEC;
		ts.tv_nsec = 0;

		int sr = pselect(s + 1, &rfds, NULL, NULL, &ts, NULL);

		if (sr < 0) {
			fprintf(stderr, "ping: pselect failed: errno=%d %s\n", errno,
					strerror(errno));
			return -1;
		}

		if (sr == 0) {
			fprintf(stderr, "ping: pselect timed out\n");
			return -1;
		}

		if (!FD_ISSET(s, &rfds)) {
			fprintf(stderr, "ping: pselect returned %d but fd is not set\n",
					sr);
			return -1;
		}

		ssize_t n = recvfrom(s, recv_buf, sizeof(recv_buf), 0,
							 (struct sockaddr *)&from, &from_len);
		if (n < 0) {
			fprintf(stderr, "ping: recvfrom failed: errno=%d %s\n", errno,
					strerror(errno));
			return -1;
		}

		clock_gettime(CLOCK_MONOTONIC, &end);

		if (ping_time_diff_us(start, &end) > PING_TIMEOUT_SEC * 1000000L)
			return -1;

		if (ping_parse_reply(recv_buf, n, &icmp, &ttl) < 0)
			continue;

		if (icmp->type == ICMP_ECHO)
			continue;

		if (icmp->type == ICMP_ECHOREPLY && icmp->code == 0 &&
			ntohs(icmp->un.echo.id) == ident &&
			ntohs(icmp->un.echo.sequence) == seq) {
			if (rtt_us_out)
				*rtt_us_out = ping_time_diff_us(start, &end);

			if (ttl_out)
				*ttl_out = ttl;

			return 0;
		}
	}

	return -1;
}

int sh_builtin_ping(int argc, char **argv)
{
	if (argc < 2 || strcmp(argv[1], "--help") == 0) {
		puts("usage: ping [-c count] ipv4-address");
		return argc < 2 ? 2 : 0;
	}

	int count = 4;
	const char *target = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "ping: -c requires a count\n");
				return 2;
			}

			count = atoi(argv[++i]);
			if (count <= 0)
				count = 1;
			continue;
		}

		if (argv[i][0] == '-' && argv[i][1]) {
			fprintf(stderr, "ping: unsupported option: %s\n", argv[i]);
			return 2;
		}

		target = argv[i];
	}

	if (!target) {
		fprintf(stderr, "ping: missing host\n");
		return 2;
	}

	struct in_addr ip;

	if (ping_parse_ipv4(target, &ip) < 0) {
		fprintf(stderr, "ping: failed to resolve %s\n", target);
		return 1;
	}

	char ipstr[INET_ADDRSTRLEN];

	if (!inet_ntop(AF_INET, &ip, ipstr, sizeof(ipstr))) {
		fprintf(stderr, "ping: inet_ntop: %s\n", strerror(errno));
		return 1;
	}

	int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);

	if (s < 0) {
		fprintf(stderr, "ping: socket: %s\n", strerror(errno));
		return 1;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr = ip;

	unsigned short ident = (unsigned short)(getpid() & 0xffff);
	int transmitted = 0;
	int received = 0;

	printf("ping: PING %s (%s): %d data bytes\n", target, ipstr,
		   PING_DATA_SIZE);

	for (int seq = 1; seq <= count; seq++) {
		struct timespec start;
		long rtt_us = 0;
		int ttl = -1;

		transmitted++;

		if (ping_send_packet(s, &addr, ident, seq, &start) < 0) {
			printf("Request failed for icmp_seq %d\n", seq);
			continue;
		}

		if (ping_recv_packet(s, ident, seq, &rtt_us, &ttl, &start) == 0) {
			received++;

			if (ttl >= 0) {
				printf(
					"%d bytes from %s: icmp_seq=%d ttl=%d time=%ld.%03ld ms\n",
					PING_DATA_SIZE + 8, ipstr, seq, ttl, rtt_us / 1000,
					rtt_us % 1000);
			} else {
				printf("%d bytes from %s: icmp_seq=%d time=%ld.%03ld ms\n",
					   PING_DATA_SIZE + 8, ipstr, seq, rtt_us / 1000,
					   rtt_us % 1000);
			}
		} else {
			printf("Request timeout for icmp_seq %d\n", seq);
		}
	}

	close(s);

	printf("--- %s ping statistics ---\n", target);
	printf("%d packets transmitted, %d packets received, %d%% packet loss\n",
		   transmitted, received,
		   transmitted == 0 ? 100 :
							  ((transmitted - received) * 100) / transmitted);

	return received > 0 ? 0 : 1;
}

int sh_is_builtin_name(const char *name)
{
	static const char *const builtin_names[] = {
		"cat",		"cd",	  "clear",	 "echo",	"env",	 "exit",  "export",
		"false",	"help",	  "hexdump", "history", "id",	 "kill",  "ls",
		"loadkeys", "mkdir",  "pgrep",	 "pidof",	"pinfo", "ping",  "printf",
		"ps",		"pwd",	  "read",	 "rm",		"rmdir", "set",	  "source",
		".",		"stat",	  "touch",	 "true",	"type",	 "uname", "unset",
		"which",	"whoami", "nfetch",	 NULL,
	};

	for (size_t i = 0; builtin_names[i]; i++) {
		if (strcmp(name, builtin_names[i]) == 0)
			return 1;
	}

	return 0;
}