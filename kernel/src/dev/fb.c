#include <dev/fb.h>

#include <errno.h>
#include <fs/devfs.h>
#include <lib/lyrterm.h>
#include <lyr/fb.h>
#include <sys/poll.h>

static int fbdev_read(void *ctx, uint64_t off, void *buf, size_t len,
					  size_t *done)
{
	(void)ctx;
	return lyrterm_framebuffer_read(off, buf, len, done);
}

static int fbdev_write(void *ctx, uint64_t off, const void *buf, size_t len,
					   size_t *done)
{
	(void)ctx;
	return lyrterm_framebuffer_write(off, buf, len, done);
}

static int fbdev_ioctl(void *ctx, unsigned long request, void *arg)
{
	(void)ctx;

	if (request != LYR_FBIOGET_INFO)
		return -ENOTTY;
	if (!arg)
		return -EINVAL;

	lyrterm_framebuffer_info_t info;
	int r = lyrterm_get_framebuffer_info(&info);
	if (r != 0)
		return r;

	lyr_fb_info_t *out = arg;
	out->width = info.width;
	out->height = info.height;
	out->pitch = info.pitch;
	out->bpp = info.bpp;
	out->size = info.size;
	return 0;
}

static int fbdev_poll(void *ctx, int events)
{
	(void)ctx;
	int revents = 0;

	if (events & LYR_POLL_READ_MASK)
		revents |= LYR_POLLIN | LYR_POLLRDNORM;
	if (events & LYR_POLL_WRITE_MASK)
		revents |= LYR_POLLOUT | LYR_POLLWRNORM;
	return revents;
}

int fbdev_init(void)
{
	int r = devfs_register_chr_poll(LYR_FB_DEVICE, 0666, fbdev_read, fbdev_write,
									fbdev_ioctl, fbdev_poll, NULL);
	if (r != 0 && r != -EEXIST)
		return r;
	return 0;
}
