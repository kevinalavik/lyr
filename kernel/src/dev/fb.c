#include <dev/fb.h>
#include <errno.h>
#include <fs/devfs.h>
#include <lib/align.h>
#include <lib/lyrterm.h>
#include <sys/poll.h>
#include <lib/string.h>
#include <mm/page.h>
#include <mm/vmm.h>
#include <debug/log.h>
#include <sched/sched.h>

static int fbdev_read(void *ctx, uint64_t off, void *buf, size_t len,
					  size_t *done)
{
	(void)ctx;

	if (done)
		*done = 0;
	if (len == 0)
		return 0;
	if (!buf)
		return -EINVAL;

	return lyrterm_framebuffer_read(off, buf, len, done);
}

static int fbdev_write(void *ctx, uint64_t off, const void *buf, size_t len,
					   size_t *done)
{
	(void)ctx;

	if (done)
		*done = 0;
	if (len == 0)
		return 0;
	if (!buf)
		return -EINVAL;

	lyrterm_framebuffer_acquire();
	return lyrterm_framebuffer_write(off, buf, len, done);
}

static int fbdev_close(void *ctx)
{
	(void)ctx;

	lyrterm_framebuffer_release();
	return 0;
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

static uint64_t fbdev_mmap(void *ctx, vas_t *vas, uint64_t hint, uint64_t off,
						   size_t len, uint64_t flags)
{
	(void)ctx;

	if (!vas || !len)
		return 0;
	if ((off & (PAGE_SIZE - 1)) != 0)
		return 0;

	lyrterm_framebuffer_info_t info;
	int r = lyrterm_get_framebuffer_info(&info);
	if (r != 0 || !info.address)
		return 0;

	uint64_t map_limit = ALIGN_UP((uint64_t)info.size, PAGE_SIZE);
	if (off >= map_limit || len > map_limit - off)
		return 0;

	lyrterm_framebuffer_acquire();
	return vas_map_phys(vas, hint, VIRT_TO_PHYS(info.address) + off, len, flags);
}

int fbdev_init(void)
{
	int r = devfs_register_chr_mmap_poll_close(LYR_FB_DEVICE, 0666, fbdev_read,
											   fbdev_write, fbdev_ioctl,
											   fbdev_poll, fbdev_close,
											   fbdev_mmap, NULL);
	if (r != 0 && r != -EEXIST)
		return r;

	lyrterm_framebuffer_info_t info;
	r = lyrterm_get_framebuffer_info(&info);
	if (r != 0)
		return r;

	return devfs_set_size(LYR_FB_DEVICE, info.size);
}
