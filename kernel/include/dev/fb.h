#ifndef _LYR_DEV_FB_H
#define _LYR_DEV_FB_H

#include <stdint.h>

#define LYR_FBIOGET_INFO 0x4600UL
#define LYR_FB_DEVICE "/dev/fb0"

typedef struct lyr_fb_info {
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t bpp;
	uint32_t size;
} lyr_fb_info_t;

int fbdev_init(void);

#endif /* _LYR_DEV_FB_H */
