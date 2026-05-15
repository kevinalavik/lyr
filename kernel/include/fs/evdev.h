#ifndef _LYR_FS_EVDEV_H
#define _LYR_FS_EVDEV_H

#include <stddef.h>
#include <stdint.h>

#include <dev/input.h>
#include <dev/kbd.h>
#include <fs/vfs.h>

typedef struct evdev evdev_t;

typedef enum evdev_kind {
	EVDEV_KIND_KEYBOARD = 0,
	EVDEV_KIND_MOUSE = 1,
} evdev_kind_t;

typedef int (*evdev_ioctl_t)(void *ctx, unsigned long request, void *arg);

int evdev_init(void);
int evdev_create(evdev_t **out, evdev_kind_t kind, evdev_ioctl_t ioctl,
				 void *ctx);
int evdev_bind_path(evdev_t *dev, const char *path, vfs_mode_t mode);
void evdev_flush(evdev_t *dev);
int evdev_push(evdev_t *dev, const void *record);
int evdev_read_record(evdev_t *dev, void *record);
int evdev_read_bytes(evdev_t *dev, void *buf, size_t len, size_t *done);

#endif /* _LYR_FS_EVDEV_H */
