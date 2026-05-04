#include <lyr/syscall.h>

#define STDOUT_FD 1

#define DEVFS_NAME "dev"
#define DEVFS_PATH "/dev"
#define DEVFS_TYPE "devfs"

#define TMPFS_NAME "tmp"
#define TMPFS_PATH "/tmp"
#define TMPFS_TYPE "tmpfs"

#define NET_DEVICES_PATH "/dev/net/devices"
#define NET_INFO_PREFIX "/dev/net/"
#define NET_INFO_SUFFIX ".info"

#define BUF_SIZE 256
#define LINE_SIZE 256
#define PATH_SIZE 128
#define VALUE_SIZE 64

#define INIT_PREFIX "init: "

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

static void print_iface_summary(const struct iface_info *info)
{
	write_raw(INIT_PREFIX);

	write_raw(info->name[0] ? info->name : "?");

	write_raw(" ");

	write_raw("link=");
	write_raw(info->link[0] ? info->link : "?");

	write_raw(" ip=");
	write_raw(info->ip[0] ? info->ip : "none");

	write_raw(" gw=");
	write_raw(info->gateway[0] ? info->gateway : "none");

	write_raw(" mtu=");
	write_raw(info->mtu[0] ? info->mtu : "?");

	write_raw(" dhcp=");
	write_raw(info->dhcp[0] ? info->dhcp : "?");

	if (info->def[0] && cstr_eq(info->def, "yes"))
		write_raw(" default");

	if (info->loopback[0] && cstr_eq(info->loopback, "yes"))
		write_raw(" loopback");

	write_raw("\n");
}

static void read_iface_info(const char *path)
{
	int fd = lyr_open(path, LYR_VFS_O_RDONLY, 0);
	if (fd < 0) {
		write_raw("  - ");
		write_raw(path);
		write_line("  unavailable");
		return;
	}

	struct iface_info info;
	char buf[BUF_SIZE];
	char line[LINE_SIZE];
	unsigned long line_len = 0;
	long n;

	iface_info_init(&info);

	while ((n = lyr_read(fd, buf, sizeof(buf))) > 0) {
		for (long i = 0; i < n; i++) {
			char c = buf[i];

			if (c == '\n') {
				line[line_len] = 0;
				parse_info_line(&info, line);
				line_len = 0;
				continue;
			}

			if (line_len + 1 < sizeof(line))
				line[line_len++] = c;
		}
	}

	if (line_len) {
		line[line_len] = 0;
		parse_info_line(&info, line);
	}

	if (n < 0)
		write_line("  ! failed to read interface info");
	else
		print_iface_summary(&info);

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

static void scan_network_interfaces(void)
{
	int fd = lyr_open(NET_DEVICES_PATH, LYR_VFS_O_RDONLY, 0);
	if (fd < 0) {
		init_msg("network: no interface list");
		return;
	}

	char buf[BUF_SIZE];
	char line[LINE_SIZE];
	unsigned long line_len = 0;
	long n;

	init_msg("network interfaces:");

	while ((n = lyr_read(fd, buf, sizeof(buf))) > 0) {
		for (long i = 0; i < n; i++) {
			char c = buf[i];

			if (c == '\n') {
				handle_netdev_line(line, line_len);
				line_len = 0;
				continue;
			}

			if (line_len + 1 < sizeof(line))
				line[line_len++] = c;
		}
	}

	if (line_len)
		handle_netdev_line(line, line_len);

	if (n < 0)
		init_msg("network: failed to read interface list");

	lyr_close(fd);
}

static void print_file(const char *path)
{
	int fd = lyr_open(path, LYR_VFS_O_RDONLY, 0);
	if (fd < 0) {
		init_msg("failed to open file");
		return;
	}

	char buf[BUF_SIZE];
	long n;

	while ((n = lyr_read(fd, buf, sizeof(buf))) > 0)
		lyr_write(STDOUT_FD, buf, n);

	if (n < 0)
		init_msg("read error");

	lyr_close(fd);
}

static void mount_fs(const char *name, const char *path, const char *type)
{
	long r = lyr_mount(name, path, type, 0, 0);

	if (r < 0) {
		write_raw(INIT_PREFIX);
		write_raw("failed to mount ");
		write_raw(type);
		write_raw(" on ");
		write_line(path);
		return;
	}

	write_raw(INIT_PREFIX);
	write_raw("mounted ");
	write_raw(type);
	write_raw(" on ");
	write_line(path);
}

int main(void)
{
	init_msg("Hello, World!");

	mount_fs(DEVFS_NAME, DEVFS_PATH, DEVFS_TYPE);
	mount_fs(TMPFS_NAME, TMPFS_PATH, TMPFS_TYPE);

	scan_network_interfaces();

	write_line("");
	print_file("/etc/motd");

	return 0;
}