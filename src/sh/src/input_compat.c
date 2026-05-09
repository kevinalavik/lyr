#include <lyr/input.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

int lyr_kbd_open(lyr_kbd_t *kbd)
{
	int fd;

	if (!kbd) {
		errno = EINVAL;
		return -1;
	}

	fd = open(LYR_KBD_DEVICE, O_RDONLY);
	if (fd < 0)
		return -1;

	kbd->fd = fd;
	return 0;
}

int lyr_kbd_close(lyr_kbd_t *kbd)
{
	int r;

	if (!kbd || kbd->fd < 0) {
		errno = EINVAL;
		return -1;
	}

	r = close(kbd->fd);
	kbd->fd = -1;
	return r;
}

int lyr_kbd_set_layout(lyr_kbd_t *kbd, const char *path)
{
	if (!kbd || !path || kbd->fd < 0) {
		errno = EINVAL;
		return -1;
	}

	return ioctl(kbd->fd, LYR_KBDIOCSMAP, path);
}

int lyr_kbd_get_layout(lyr_kbd_t *kbd, char *buf)
{
	if (!kbd || !buf || kbd->fd < 0) {
		errno = EINVAL;
		return -1;
	}

	return ioctl(kbd->fd, LYR_KBDIOCGMAP, buf);
}
