#include <lyr/syscall.h>

#define OUT 1

#define NET_DEVICES "/dev/net/devices"
#define TEST_PATH "/bin/hello-world"

#define TEST_PORT 6969

#define BUF_SIZE 512
#define IFACE_SIZE 32
#define IP_SIZE 32

static unsigned long len(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;

	return n;
}

static void print(const char *s)
{
	lyr_write(OUT, s, len(s));
}

static void puts(const char *s)
{
	print(s);
	print("\n");
}

static int starts_with(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s != *prefix)
			return 0;

		s++;
		prefix++;
	}

	return 1;
}

static void copy_token(char *dst, unsigned long dstsz, const char *src)
{
	unsigned long i = 0;

	if (!dstsz)
		return;

	while (src[i] && src[i] != ' ' && src[i] != '\t' && src[i] != '\r' &&
		   src[i] != '\n' && i + 1 < dstsz) {
		dst[i] = src[i];
		i++;
	}

	dst[i] = 0;
}

static int parse_octet(const char **s, unsigned long *out)
{
	unsigned long v = 0;
	unsigned long digits = 0;

	while (**s >= '0' && **s <= '9') {
		v = v * 10 + (**s - '0');
		(*s)++;
		digits++;

		if (v > 255)
			return 0;
	}

	if (!digits)
		return 0;

	*out = v;
	return 1;
}

static int parse_ip(const char *s, unsigned long *out)
{
	unsigned long a;
	unsigned long b;
	unsigned long c;
	unsigned long d;

	if (!parse_octet(&s, &a) || *s++ != '.')
		return 0;

	if (!parse_octet(&s, &b) || *s++ != '.')
		return 0;

	if (!parse_octet(&s, &c) || *s++ != '.')
		return 0;

	if (!parse_octet(&s, &d) || *s)
		return 0;

	*out = (a << 24) | (b << 16) | (c << 8) | d;
	return 1;
}

static int probe_ip(const char *ip)
{
	unsigned long addr_ip;
	int sock;
	sockaddr_in_t addr;

	if (!parse_ip(ip, &addr_ip))
		return 0;

	sock = lyr_socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		return 0;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(TEST_PORT);
	addr.sin_addr = htonl(addr_ip);

	if (lyr_connect(sock, (sockaddr_t *)&addr, sizeof(addr)) < 0) {
		lyr_close(sock);
		return 0;
	}

	lyr_close(sock);
	return 1;
}

static void parse_iface_line(const char *line)
{
	char name[IFACE_SIZE];
	char ip[IP_SIZE];
	unsigned long i = 0;

	name[0] = 0;
	ip[0] = 0;

	while (line[i] == ' ' || line[i] == '\t')
		i++;

	copy_token(name, sizeof(name), line + i);

	while (line[i]) {
		while (line[i] == ' ' || line[i] == '\t')
			i++;

		if (starts_with(line + i, "ip=")) {
			copy_token(ip, sizeof(ip), line + i + 3);
			break;
		}

		while (line[i] && line[i] != ' ' && line[i] != '\t' &&
			   line[i] != '\r' && line[i] != '\n')
			i++;
	}

	if (!name[0] || !ip[0])
		return;

	print("init: discovered ");
	print(name);
	print(" (");
	print(ip);
	puts(")");

	if (probe_ip(ip)) {
		print("init: iface ");
		print(name);
		puts(" works");
	} else {
		print("init: iface ");
		print(name);
		puts(" failed");
	}
}

static void scan_ifaces(void)
{
	char buf[BUF_SIZE];
	char line[BUF_SIZE];
	unsigned long line_len = 0;
	long n;
	int fd;

	fd = lyr_open(NET_DEVICES, LYR_VFS_O_RDONLY, 0);
	if (fd < 0) {
		puts("init: network unavailable");
		return;
	}

	while ((n = lyr_read(fd, buf, sizeof(buf))) > 0) {
		for (long i = 0; i < n; i++) {
			char c = buf[i];

			if (c == '\n') {
				line[line_len] = 0;
				parse_iface_line(line);
				line_len = 0;
			} else if (line_len + 1 < sizeof(line)) {
				line[line_len++] = c;
			}
		}
	}

	if (line_len) {
		line[line_len] = 0;
		parse_iface_line(line);
	}

	lyr_close(fd);
}

static void mount_one(const char *name, const char *path, const char *type)
{
	if (lyr_mount(name, path, type, 0, 0) < 0) {
		print("init: mount failed ");
		puts(path);
	}
}

int main(void)
{
	puts("init: lyrOS");

	mount_one("devfs", "/dev", "devfs");
	mount_one("tmpfs", "/tmp", "tmpfs");

	scan_ifaces();

	static char path[] = TEST_PATH;
	char *const argv[] = { path, 0 };
	char *const envp[] = { 0 };

	puts("init: launching " TEST_PATH);
	lyr_execve(path, argv, envp);

	puts("init: exec failed");
	return 1;
}