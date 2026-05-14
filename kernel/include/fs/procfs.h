#ifndef _LYR_FS_PROCFS_H
#define _LYR_FS_PROCFS_H

#include <fs/vfs.h>

vfs_node_t *procfs_create_root(void);
int procfs_mount(const char *target);
int procfs_note_mount(const char *source, const char *target, const char *fstype,
					  const char *opts);

#endif /* _LYR_FS_PROCFS_H */
