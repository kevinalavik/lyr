#ifndef _LYR_UAPI_VFS_H
#define _LYR_UAPI_VFS_H

#include <stdint.h>

/* Mode bits */
#define LYR_VFS_NAME_MAX 255
#define LYR_VFS_S_IFMT 0170000u
#define LYR_VFS_S_IFREG 0100000u
#define LYR_VFS_S_IFDIR 0040000u
#define LYR_VFS_S_ISREG(m) (((m) & LYR_VFS_S_IFMT) == LYR_VFS_S_IFREG)
#define LYR_VFS_S_ISDIR(m) (((m) & LYR_VFS_S_IFMT) == LYR_VFS_S_IFDIR)

/* Access mode bits */
#define LYR_VFS_R_OK 4
#define LYR_VFS_W_OK 2
#define LYR_VFS_X_OK 1

/* Open flags */
#define LYR_VFS_O_RDONLY 0x0000u
#define LYR_VFS_O_WRONLY 0x0001u
#define LYR_VFS_O_RDWR 0x0002u
#define LYR_VFS_O_CREAT 0x0100u
#define LYR_VFS_O_EXCL 0x0200u
#define LYR_VFS_O_TRUNC 0x0400u
#define LYR_VFS_O_APPEND 0x0800u
#define LYR_VFS_O_DIRECTORY 0x1000u

/* lseek whence */
#define LYR_VFS_SEEK_SET 0
#define LYR_VFS_SEEK_CUR 1
#define LYR_VFS_SEEK_END 2

typedef struct {
	char name[LYR_VFS_NAME_MAX + 1];
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	uint64_t size;
	uint32_t nlink;
} lyr_dirent_t;

typedef struct {
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	uint64_t size;
	uint32_t nlink;
} lyr_stat_t;

#endif /* _LYR_UAPI_VFS_H */