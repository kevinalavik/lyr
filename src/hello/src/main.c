#include <errno.h>
#include <fcntl.h>
#include <lyr/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static uint32_t pack_rgb(uint32_t r, uint32_t g, uint32_t b)
{
	return (r << 16) | (g << 8) | b;
}

int main(void)
{
	int fd = open(LYR_FB_DEVICE, O_RDWR);
	if (fd < 0) {
		perror("open(/dev/fb0)");
		return 1;
	}

	lyr_fb_info_t info;
	if (ioctl(fd, LYR_FBIOGET_INFO, &info) < 0) {
		perror("ioctl(LYR_FBIOGET_INFO)");
		close(fd);
		return 1;
	}

	if (info.bpp != 32) {
		fprintf(stderr, "hello: unsupported framebuffer bpp=%u\n", info.bpp);
		close(fd);
		return 1;
	}

	uint8_t *buf = malloc(info.size);
	if (!buf) {
		fprintf(stderr, "hello: failed to allocate %u bytes\n", info.size);
		close(fd);
		return 1;
	}

	for (uint32_t y = 0; y < info.height; y++) {
		uint32_t *row = (uint32_t *)(buf + y * info.pitch);
		for (uint32_t x = 0; x < info.width; x++) {
			uint32_t r = (x * 255u) / (info.width ? info.width : 1);
			uint32_t g = (y * 255u) / (info.height ? info.height : 1);
			uint32_t b = ((x ^ y) & 0xffu);
			row[x] = pack_rgb(r, g, b);
		}
	}

	uint32_t box_w = info.width / 3;
	uint32_t box_h = info.height / 4;
	uint32_t box_x0 = (info.width - box_w) / 2;
	uint32_t box_y0 = (info.height - box_h) / 2;

	for (uint32_t y = box_y0; y < box_y0 + box_h; y++) {
		uint32_t *row = (uint32_t *)(buf + y * info.pitch);
		for (uint32_t x = box_x0; x < box_x0 + box_w; x++) {
			int border = (x - box_x0 < 6) || (box_x0 + box_w - x <= 6) ||
						 (y - box_y0 < 6) || (box_y0 + box_h - y <= 6);
			row[x] = border ? pack_rgb(255, 255, 255) : pack_rgb(20, 20, 20);
		}
	}

	ssize_t written = write(fd, buf, info.size);
	if (written < 0) {
		perror("write(/dev/fb0)");
		free(buf);
		close(fd);
		return 1;
	}

	printf("hello: drew %u bytes to %s (%ux%u pitch=%u bpp=%u)\n", info.size,
		   LYR_FB_DEVICE, info.width, info.height, info.pitch, info.bpp);

	free(buf);
	close(fd);
	return 0;
}
