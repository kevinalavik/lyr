#ifndef _LYR_FS_EXT2_H
#define _LYR_FS_EXT2_H

#include <dev/block.h>

int ext2_mount(block_device_t *dev, const char *path);

#endif /* _LYR_FS_EXT2_H */
