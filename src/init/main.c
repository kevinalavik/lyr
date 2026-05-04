#include <lyr/syscall.h>

static unsigned long cstr_len(const char *s)
{
	unsigned long len = 0;
	while (s[len])
		len++;
	return len;
}

static void write_str(const char *s)
{
	lyr_write(1, s, cstr_len(s));
}

static void puts(const char *s)
{
	write_str(s);
	write_str("\n");
}

static void print_file(const char *path)
{
	int fd = lyr_open(path, LYR_VFS_O_RDONLY, 0);
	if (fd < 0) {
		puts("init: failed to open file");
		return;
	}

	char buf[256];
	long n;

	while ((n = lyr_read(fd, buf, sizeof(buf))) > 0) {
		lyr_write(1, buf, n);
	}

	if (n < 0)
		puts("init: read error");

	lyr_close(fd);
}

int main(void)
{
	puts("init: Hello, World!");

	long r = lyr_mount("dev", "/dev", "devfs", 0, 0);
	if (r < 0)
		puts("init: failed to mount devfs on /dev");
	else
		puts("init: mounted devfs on /dev");

	r = lyr_mount("tmp", "/tmp", "tmpfs", 0, 0);
	if (r < 0)
		puts("init: failed to mount tmpfs on /tmp");
	else
		puts("init: mounted tmpfs on /tmp");

	puts("");

	/*motd*/
	print_file("/etc/motd");

	return 0;
}