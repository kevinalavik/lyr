#ifndef _LYR_FS_DEVFS_H
#define _LYR_FS_DEVFS_H

#include <stddef.h>
#include <stdint.h>
#include <fs/vfs.h>

typedef int (*devfs_read_t)(void *ctx, uint64_t off, void *buf, size_t len,
							size_t *done);
typedef int (*devfs_write_t)(void *ctx, uint64_t off, const void *buf,
							 size_t len, size_t *done);

int devfs_init(void);
int devfs_mkdir(const char *path, vfs_mode_t mode);
int devfs_register_chr(const char *path, vfs_mode_t mode, devfs_read_t read,
					   devfs_write_t write, void *ctx);
int devfs_unregister(const char *path);

#endif /* _LYR_FS_DEVFS_H */
