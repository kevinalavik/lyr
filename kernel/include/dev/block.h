#ifndef _LYR_DEV_BLOCK_H
#define _LYR_DEV_BLOCK_H

#include <stddef.h>
#include <stdint.h>
#include <fs/vfs.h>

#define BLOCK_NAME_MAX 31

typedef struct block_device block_device_t;

typedef int (*block_read_blocks_t)(block_device_t *dev, uint64_t lba,
								   uint32_t count, void *buf);
typedef int (*block_write_blocks_t)(block_device_t *dev, uint64_t lba,
									uint32_t count, const void *buf);

struct block_device {
	char name[BLOCK_NAME_MAX + 1];
	uint32_t block_size;
	uint64_t block_count;
	uint64_t lba_offset;
	block_read_blocks_t read_blocks;
	block_write_blocks_t write_blocks;
	void *driver_data;
	block_device_t *parent;
	block_device_t *next;
};

int block_system_init(void);
int block_register(block_device_t *src);
int block_read(block_device_t *dev, uint64_t off, void *buf, size_t len);
int block_write(block_device_t *dev, uint64_t off, const void *buf, size_t len);
block_device_t *block_first(void);
block_device_t *block_find(const char *name);

#endif /* _LYR_DEV_BLOCK_H */
