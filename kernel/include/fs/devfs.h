#ifndef _LYR_FS_DEVFS_H
#define _LYR_FS_DEVFS_H

#include <stddef.h>
#include <stdint.h>
#include <fs/vfs.h>

typedef int (*devfs_read_t)(void *ctx, uint64_t off, void *buf, size_t len,
							size_t *done);
typedef int (*devfs_write_t)(void *ctx, uint64_t off, const void *buf,
							 size_t len, size_t *done);
typedef int (*devfs_ioctl_t)(void *ctx, unsigned long request, void *arg);
typedef int (*devfs_poll_t)(void *ctx, int events);
typedef int (*devfs_close_t)(void *ctx);

int devfs_init(void);
int devfs_mount(const char *target);
int devfs_mkdir(const char *path, vfs_mode_t mode);

int devfs_register_chr(const char *path, vfs_mode_t mode, devfs_read_t read,
					   devfs_write_t write, void *ctx);

int devfs_register_chr_ex(const char *path, vfs_mode_t mode, devfs_read_t read,
						  devfs_write_t write, devfs_ioctl_t ioctl, void *ctx);

int devfs_register_chr_poll(const char *path, vfs_mode_t mode, devfs_read_t read,
							devfs_write_t write, devfs_ioctl_t ioctl,
							devfs_poll_t poll, void *ctx);

int devfs_register_chr_poll_close(const char *path, vfs_mode_t mode,
								  devfs_read_t read, devfs_write_t write,
								  devfs_ioctl_t ioctl, devfs_poll_t poll,
								  devfs_close_t close, void *ctx);

int devfs_set_size(const char *path, uint64_t size);
int devfs_unregister(const char *path);

#endif /* _LYR_FS_DEVFS_H */