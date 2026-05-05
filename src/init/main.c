#include <lyr/syscall.h>

#define STDOUT_FD 1

#define DEVFS_NAME "dev"
#define DEVFS_PATH "/dev"
#define DEVFS_TYPE "devfs"

#define TMPFS_NAME "tmp"
#define TMPFS_PATH "/tmp"
#define TMPFS_TYPE "tmpfs"

#define MOTD_PATH "/etc/motd"
#define TEST_PATH "/bin/hello-world"

#define NET_DEVICES_PATH "/dev/net/devices"
#define NET_ROUTES_PATH "/dev/net/routes"
#define NET_INFO_PREFIX "/dev/net/"
#define NET_INFO_SUFFIX ".info"

#define INIT_PREFIX "init: "

#define BUF_SIZE 256
#define LINE_SIZE 256
#define PATH_SIZE 128
#define VALUE_SIZE 64

#define YES "yes"

struct iface_info {
	char name[VALUE_SIZE];
	char ip[VALUE_SIZE];
	char gateway[VALUE_SIZE];
	char mtu[VALUE_SIZE];
	char dhcp[VALUE_SIZE];
	char link[VALUE_SIZE];
	char def[VALUE_SIZE];
	char loopback[VALUE_SIZE];
};

static unsigned long cstr_len(const char *s)
{
	unsigned long len = 0;

	while (s[len])
		len++;

	return len;
}

static int cstr_eq(const char *a, const char *b)
{
	while (*a && *b) {
		if (*a != *b)
			return 0;
		a++;
		b++;
	}

	return *a == *b;
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

static void write_raw(const char *s)
{
	lyr_write(STDOUT_FD, s, cstr_len(s));
}

static void write_line(const char *s)
{
	write_raw(s);
	write_raw("\n");
}

static void init_raw(const char *topic)
{
	write_raw(INIT_PREFIX);
	write_raw(topic);
	write_raw(": ");
}

static void init_msg(const char *s)
{
	write_raw(INIT_PREFIX);
	write_line(s);
}

static void copy_value(char *dst, unsigned long dstsz, const char *src)
{
	unsigned long i = 0;

	if (!dstsz)
		return;

	while (src[i] && src[i] != '\n' && src[i] != '\r' && i + 1 < dstsz) {
		dst[i] = src[i];
		i++;
	}

	dst[i] = 0;
}

static void iface_info_init(struct iface_info *info)
{
	info->name[0] = 0;
	info->ip[0] = 0;
	info->gateway[0] = 0;
	info->mtu[0] = 0;
	info->dhcp[0] = 0;
	info->link[0] = 0;
	info->def[0] = 0;
	info->loopback[0] = 0;
}

static void parse_info_line(struct iface_info *info, const char *line)
{
	if (starts_with(line, "name="))
		copy_value(info->name, sizeof(info->name), line + 5);
	else if (starts_with(line, "ip="))
		copy_value(info->ip, sizeof(info->ip), line + 3);
	else if (starts_with(line, "gateway="))
		copy_value(info->gateway, sizeof(info->gateway), line + 8);
	else if (starts_with(line, "mtu="))
		copy_value(info->mtu, sizeof(info->mtu), line + 4);
	else if (starts_with(line, "dhcp="))
		copy_value(info->dhcp, sizeof(info->dhcp), line + 5);
	else if (starts_with(line, "link="))
		copy_value(info->link, sizeof(info->link), line + 5);
	else if (starts_with(line, "default="))
		copy_value(info->def, sizeof(info->def), line + 8);
	else if (starts_with(line, "loopback="))
		copy_value(info->loopback, sizeof(info->loopback), line + 9);
}

static void write_pad_name(const char *s)
{
	unsigned long len = cstr_len(s);

	write_raw(s);

	while (len < 5) {
		write_raw(" ");
		len++;
	}
}

static void print_iface_summary(const struct iface_info *info)
{
	init_raw("net");

	write_pad_name(info->name[0] ? info->name : "?");

	write_raw(" ");
	write_raw(info->link[0] ? info->link : "?");

	write_raw(" ip ");
	write_raw(info->ip[0] ? info->ip : "none");

	write_raw(" mtu ");
	write_raw(info->mtu[0] ? info->mtu : "?");

	write_raw(" ");
	write_raw((info->dhcp[0] && cstr_eq(info->dhcp, YES)) ? "dhcp" : "static");

	if (info->def[0] && cstr_eq(info->def, YES))
		write_raw(" default");

	if (info->gateway[0] && !cstr_eq(info->gateway, "0.0.0.0")) {
		write_raw(" gw ");
		write_raw(info->gateway);
	}

	if (info->loopback[0] && cstr_eq(info->loopback, YES))
		write_raw(" loopback");

	write_raw("\n");
}

static void make_iface_info_path(char *dst, unsigned long dstsz,
								 const char *name, unsigned long name_len)
{
	unsigned long i = 0;
	unsigned long j;

	for (j = 0; NET_INFO_PREFIX[j] && i + 1 < dstsz; j++)
		dst[i++] = NET_INFO_PREFIX[j];

	for (j = 0; j < name_len && i + 1 < dstsz; j++)
		dst[i++] = name[j];

	for (j = 0; NET_INFO_SUFFIX[j] && i + 1 < dstsz; j++)
		dst[i++] = NET_INFO_SUFFIX[j];

	dst[i] = 0;
}

static void read_iface_info(const char *path)
{
	struct iface_info info;
	char buf[BUF_SIZE];
	char line[LINE_SIZE];
	unsigned long line_len = 0;
	long n;
	int fd;

	fd = lyr_open(path, LYR_VFS_O_RDONLY, 0);
	if (fd < 0) {
		init_raw("net");
		write_raw(path);
		write_line(" unavailable");
		return;
	}

	iface_info_init(&info);

	while ((n = lyr_read(fd, buf, sizeof(buf))) > 0) {
		for (long i = 0; i < n; i++) {
			char c = buf[i];

			if (c == '\n') {
				line[line_len] = 0;
				parse_info_line(&info, line);
				line_len = 0;
			} else if (line_len + 1 < sizeof(line)) {
				line[line_len++] = c;
			}
		}
	}

	if (line_len) {
		line[line_len] = 0;
		parse_info_line(&info, line);
	}

	if (n < 0) {
		init_raw("net");
		write_line("failed to read interface info");
	} else {
		print_iface_summary(&info);
	}

	lyr_close(fd);
}

static void handle_netdev_line(const char *line, unsigned long len)
{
	unsigned long i = 0;
	unsigned long name_start;
	unsigned long name_len;
	char path[PATH_SIZE];

	while (i < len && (line[i] == ' ' || line[i] == '\t'))
		i++;

	name_start = i;

	while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' &&
		   line[i] != '\n')
		i++;

	name_len = i - name_start;
	if (!name_len)
		return;

	make_iface_info_path(path, sizeof(path), line + name_start, name_len);
	read_iface_info(path);
}

static void read_lines(const char *path,
					   void (*line_cb)(const char *line, unsigned long len))
{
	char buf[BUF_SIZE];
	char line[LINE_SIZE];
	unsigned long line_len = 0;
	long n;
	int fd;

	fd = lyr_open(path, LYR_VFS_O_RDONLY, 0);
	if (fd < 0)
		return;

	while ((n = lyr_read(fd, buf, sizeof(buf))) > 0) {
		for (long i = 0; i < n; i++) {
			char c = buf[i];

			if (c == '\n') {
				line_cb(line, line_len);
				line_len = 0;
			} else if (line_len + 1 < sizeof(line)) {
				line[line_len++] = c;
			}
		}
	}

	if (line_len)
		line_cb(line, line_len);

	lyr_close(fd);
}

static unsigned long token_len(const char *s, unsigned long max)
{
	unsigned long i = 0;

	while (i < max && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' &&
		   s[i] != '\n')
		i++;

	return i;
}

static void write_token(const char *s, unsigned long len)
{
	lyr_write(STDOUT_FD, s, len);
}

static void print_route_line(const char *line, unsigned long len)
{
	unsigned long i = 0;
	unsigned long dst_len;

	while (i < len && (line[i] == ' ' || line[i] == '\t'))
		i++;

	if (i >= len)
		return;

	dst_len = token_len(line + i, len - i);

	init_raw("route");

	if (dst_len == 15 && starts_with(line + i, "0.0.0.0/0.0.0.0")) {
		write_raw("default");
	} else {
		write_token(line + i, dst_len);
	}

	i += dst_len;

	while (i < len) {
		if (line[i] == '\r' || line[i] == '\n')
			break;

		write_raw(" ");
		i++;

		while (i < len && (line[i] == ' ' || line[i] == '\t'))
			i++;

		unsigned long tlen = token_len(line + i, len - i);
		if (!tlen)
			break;

		if (tlen == 7 && starts_with(line + i, "default")) {
			i += tlen;
			continue;
		}

		write_token(line + i, tlen);
		i += tlen;
	}

	write_raw("\n");
}

static void scan_network(void)
{
	init_msg("network: configure");

	read_lines(NET_DEVICES_PATH, handle_netdev_line);
	read_lines(NET_ROUTES_PATH, print_route_line);

	init_msg("network: ready");
}

static void print_file(const char *path)
{
	char buf[BUF_SIZE];
	long n;
	int fd;

	fd = lyr_open(path, LYR_VFS_O_RDONLY, 0);
	if (fd < 0) {
		init_msg("motd: unavailable");
		return;
	}

	while ((n = lyr_read(fd, buf, sizeof(buf))) > 0)
		lyr_write(STDOUT_FD, buf, n);

	if (n < 0)
		init_msg("motd: read error");

	lyr_close(fd);
}

static void mount_fs(const char *name, const char *path, const char *type)
{
	long r;

	r = lyr_mount(name, path, type, 0, 0);
	if (r < 0) {
		write_raw(INIT_PREFIX);
		write_raw("mount: ");
		write_raw(type);
		write_raw(" on ");
		write_raw(path);
		write_line(" failed");
		return;
	}

	write_raw(INIT_PREFIX);
	write_raw("mount: ");
	write_raw(type);
	write_raw(" on ");
	write_raw(path);
	write_line(" ok");
}

int main(void)
{
	init_msg("lyrOS init");

	mount_fs(DEVFS_NAME, DEVFS_PATH, DEVFS_TYPE);
	mount_fs(TMPFS_NAME, TMPFS_PATH, TMPFS_TYPE);

	scan_network();

	write_line("");
	print_file(MOTD_PATH);

	init_msg("exec: /bin/hello-world");
	static char test_path[] = TEST_PATH;
	char *const argv[] = { test_path, 0 };
	char *const envp[] = { 0 };
	if (lyr_execve(test_path, argv, envp) < 0)
		init_msg("exec: failed");

	return 0;
}
