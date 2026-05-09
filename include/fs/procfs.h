#ifndef _LYR_FS_PROCFS_H
#define _LYR_FS_PROCFS_H

#include <fs/vfs.h>

vfs_node_t *procfs_create_root(void);
int procfs_mount(const char *target);

#endif /* _LYR_FS_PROCFS_H */
