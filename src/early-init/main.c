#include <lyr/syscall.h>

static unsigned long cstr_len(const char *s)
{
	unsigned long len = 0;
	while (s[len])
		len++;
	return len;
}

static void puts(const char *s)
{
	lyr_write(1, s, cstr_len(s));
	lyr_write(1, "\n", 1);
}

static void die(const char *msg, long err)
{
	lyr_write(2, msg, cstr_len(msg));
	lyr_write(2, "\n", 1);
	lyr_exit((int)-err);
}

int main(void)
{
	long ret;

	puts("early-init: starting");
	ret = lyr_change_root("/dev/nvme0n1", "ext2", "/bin/init");
	if (ret < 0)
		die("early-init: change_root /dev/nvme0n1 failed", ret);

	puts("early-init: change_root unexpectedly returned");
	return 0;
}