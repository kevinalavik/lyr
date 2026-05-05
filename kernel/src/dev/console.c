#include <dev/console.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <lib/lyrterm.h>
#include <lib/string.h>
#include <util/kprintf.h>

#define LYR_NCCS 19

typedef struct lyr_termios {
	uint32_t c_iflag;
	uint32_t c_oflag;
	uint32_t c_cflag;
	uint32_t c_lflag;
	uint8_t c_line;
	uint8_t c_cc[LYR_NCCS];
	uint32_t c_ispeed;
	uint32_t c_ospeed;
} lyr_termios_t;

static int console_read(void *ctx, uint64_t off, void *buf, size_t len,
						size_t *done)
{
	(void)ctx;
	(void)off;
	(void)buf;
	(void)len;

	if (done)
		*done = 0;

	return VFS_OK;
}

static int console_write(void *ctx, uint64_t off, const void *buf, size_t len,
						 size_t *done)
{
	(void)ctx;
	(void)off;

	if (!buf)
		return VFS_ERR_INVAL;

	size_t pos = 0;
	while (pos < len) {
		size_t chunk = len - pos;
		if (chunk > 255)
			chunk = 255;

		char tmp[256];
		memcpy(tmp, (const char *)buf + pos, chunk);
		tmp[chunk] = '\0';

		kprintf("%s", tmp);
		pos += chunk;
	}

	if (done)
		*done = len;

	return VFS_OK;
}

static int console_get_winsize(lyr_winsize_t *ws)
{
	if (!ws)
		return VFS_ERR_INVAL;

	uint32_t cols = 0;
	uint32_t rows = 0;
	uint32_t width = 0;
	uint32_t height = 0;

	lyrterm_get_size(&cols, &rows, &width, &height);

	ws->ws_row = (uint16_t)rows;
	ws->ws_col = (uint16_t)cols;
	ws->ws_xpixel = (uint16_t)width;
	ws->ws_ypixel = (uint16_t)height;

	return VFS_OK;
}

static int console_ioctl(void *ctx, unsigned long request, void *arg)
{
	(void)ctx;

	switch (request) {
	case LYR_TCGETS: {
		if (!arg)
			return VFS_ERR_INVAL;

		lyr_termios_t tio;
		memset(&tio, 0, sizeof(tio));
		memcpy(arg, &tio, sizeof(tio));

		return VFS_OK;
	}

	case LYR_TIOCGWINSZ:
		return console_get_winsize((lyr_winsize_t *)arg);

	case LYR_TIOCSWINSZ:
		return arg ? VFS_OK : VFS_ERR_INVAL;

	default:
		return VFS_ERR_NOTTY;
	}
}

int console_init(void)
{
	int r = devfs_register_chr_ex("/dev/console", 0666, console_read,
								  console_write, console_ioctl, NULL);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;

	r = devfs_register_chr_ex("/dev/tty", 0666, console_read, console_write,
							  console_ioctl, NULL);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;

	r = devfs_register_chr_ex("/dev/stdout", 0222, NULL, console_write,
							  console_ioctl, NULL);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;

	r = devfs_register_chr_ex("/dev/stderr", 0222, NULL, console_write,
							  console_ioctl, NULL);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;

	return VFS_OK;
}