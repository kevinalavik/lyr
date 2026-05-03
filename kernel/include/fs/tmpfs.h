#ifndef _LYR_FS_TMPFS_H
#define _LYR_FS_TMPFS_H

#include <fs/vfs.h>

vfs_node_t *tmpfs_create_root(vfs_mode_t mode, vfs_uid_t uid, vfs_gid_t gid);

#endif /* _LYR_FS_TMPFS_H */
