#ifndef _LYR_FS_PIPE_H
#define _LYR_FS_PIPE_H

#include <fs/vfs.h>
#include <stddef.h>

int vfs_pipe_create(vfs_file_t **read_end, vfs_file_t **write_end);
int vfs_pipe_is(vfs_file_t *file);
int vfs_pipe_ref(vfs_file_t *file);
int vfs_pipe_read(vfs_file_t *file, void *buf, size_t len, size_t *done);
int vfs_pipe_write(vfs_file_t *file, const void *buf, size_t len, size_t *done);
int vfs_pipe_seek(vfs_file_t *file, int whence, int64_t off, uint64_t *new_off);

#endif /* _LYR_FS_PIPE_H */
