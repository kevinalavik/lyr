#ifndef _LYR_FS_MOUNT_H
#define _LYR_FS_MOUNT_H

#include <stdint.h>

int fs_mount_spec(const char *source, const char *target, const char *fstype,
				  uint64_t flags, const char *data);

#endif /* _LYR_FS_MOUNT_H */
