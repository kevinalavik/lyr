#ifndef _LYR_FS_VFS_H
#define _LYR_FS_VFS_H

#include <stddef.h>
#include <stdint.h>
#include <mm/page.h>

#define VFS_NAME_MAX 255
#define VFS_SUPP_GROUP_MAX 8

#define VFS_S_IFMT 0170000u
#define VFS_S_IFSOCK 0140000u
#define VFS_S_IFLNK 0120000u
#define VFS_S_IFREG 0100000u
#define VFS_S_IFBLK 0060000u
#define VFS_S_IFDIR 0040000u
#define VFS_S_IFCHR 0020000u
#define VFS_S_IFIFO 0010000u

#define VFS_S_ISUID 04000u
#define VFS_S_ISGID 02000u
#define VFS_S_ISVTX 01000u
#define VFS_S_IRUSR 00400u
#define VFS_S_IWUSR 00200u
#define VFS_S_IXUSR 00100u
#define VFS_S_IRGRP 00040u
#define VFS_S_IWGRP 00020u
#define VFS_S_IXGRP 00010u
#define VFS_S_IROTH 00004u
#define VFS_S_IWOTH 00002u
#define VFS_S_IXOTH 00001u
#define VFS_S_PERM 07777u

#define VFS_S_ISREG(m) (((m) & VFS_S_IFMT) == VFS_S_IFREG)
#define VFS_S_ISDIR(m) (((m) & VFS_S_IFMT) == VFS_S_IFDIR)

#define VFS_R_OK 4
#define VFS_W_OK 2
#define VFS_X_OK 1

#define VFS_O_RDONLY 0x0000u
#define VFS_O_WRONLY 0x0001u
#define VFS_O_RDWR 0x0002u
#define VFS_O_ACCMODE 0x0003u
#define VFS_O_CREAT 0x0100u
#define VFS_O_EXCL 0x0200u
#define VFS_O_TRUNC 0x0400u
#define VFS_O_APPEND 0x0800u
#define VFS_O_DIRECTORY 0x1000u

#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

#define VFS_OK 0
#define VFS_ERR_PERM -1
#define VFS_ERR_NOENT -2
#define VFS_ERR_BADF -9
#define VFS_ERR_NOMEM -12
#define VFS_ERR_ACCES -13
#define VFS_ERR_EXIST -17
#define VFS_ERR_NOTDIR -20
#define VFS_ERR_ISDIR -21
#define VFS_ERR_INVAL -22
#define VFS_ERR_NOSYS -38
#define VFS_ERR_NOTEMPTY -39
#define VFS_ERR_NAMETOOLONG -36

typedef uint32_t vfs_mode_t;
typedef uint32_t vfs_uid_t;
typedef uint32_t vfs_gid_t;

typedef struct vfs_node vfs_node_t;
typedef struct vfs_file vfs_file_t;
typedef struct vfs_ops vfs_ops_t;

typedef struct {
	vfs_uid_t uid;
	vfs_gid_t gid;
	vfs_gid_t groups[VFS_SUPP_GROUP_MAX];
	size_t group_count;
	vfs_mode_t umask;
} vfs_cred_t;

typedef struct {
	vfs_mode_t mode;
	vfs_uid_t uid;
	vfs_gid_t gid;
	uint64_t size;
	uint32_t nlink;
} vfs_stat_t;

struct vfs_ops {
	int (*lookup)(vfs_node_t *dir, const char *name, size_t len,
				  vfs_node_t **out);
	int (*create)(vfs_node_t *dir, const char *name, size_t len,
				  vfs_mode_t mode, const vfs_cred_t *cred, vfs_node_t **out);
	int (*mkdir)(vfs_node_t *dir, const char *name, size_t len, vfs_mode_t mode,
				 const vfs_cred_t *cred, vfs_node_t **out);
	int (*unlink)(vfs_node_t *dir, const char *name, size_t len);
	int (*rmdir)(vfs_node_t *dir, const char *name, size_t len);
	int (*read)(vfs_node_t *node, uint64_t off, void *buf, size_t len,
				size_t *done);
	int (*write)(vfs_node_t *node, uint64_t off, const void *buf, size_t len,
				 size_t *done);
	int (*truncate)(vfs_node_t *node, uint64_t size);
	int (*get_page)(vfs_node_t *node, uint64_t page_index, int for_write,
					page_t **out);
	void (*release)(vfs_node_t *node);
};

struct vfs_node {
	const vfs_ops_t *ops;
	vfs_mode_t mode;
	vfs_uid_t uid;
	vfs_gid_t gid;
	uint64_t size;
	uint32_t nlink;
	uint32_t refs;
	void *private_data;
};

struct vfs_file {
	vfs_node_t *node;
	uint32_t flags;
	uint64_t offset;
	vfs_cred_t cred;
};

extern const vfs_cred_t vfs_root_cred;

void vfs_node_init(vfs_node_t *node, const vfs_ops_t *ops, vfs_mode_t mode,
				   vfs_uid_t uid, vfs_gid_t gid);
void vfs_node_ref(vfs_node_t *node);
void vfs_node_release(vfs_node_t *node);

void vfs_init(vfs_node_t *root);
vfs_node_t *vfs_root(void);
int vfs_resolve(const char *path, const vfs_cred_t *cred, vfs_node_t **out);
int vfs_open(const char *path, uint32_t flags, vfs_mode_t mode,
			 const vfs_cred_t *cred, vfs_file_t **out);
int vfs_close(vfs_file_t *file);
int vfs_read(vfs_file_t *file, void *buf, size_t len, size_t *done);
int vfs_write(vfs_file_t *file, const void *buf, size_t len, size_t *done);
int vfs_seek(vfs_file_t *file, int whence, int64_t off, uint64_t *new_off);
int vfs_mkdir(const char *path, vfs_mode_t mode, const vfs_cred_t *cred);
int vfs_unlink(const char *path, const vfs_cred_t *cred);
int vfs_rmdir(const char *path, const vfs_cred_t *cred);
int vfs_chmod(const char *path, vfs_mode_t mode, const vfs_cred_t *cred);
int vfs_chown(const char *path, vfs_uid_t uid, vfs_gid_t gid,
			  const vfs_cred_t *cred);
int vfs_stat(const char *path, const vfs_cred_t *cred, vfs_stat_t *st);
int vfs_access(vfs_node_t *node, const vfs_cred_t *cred, int mask);
int vfs_node_get_page(vfs_node_t *node, uint64_t page_index, int for_write,
					  page_t **out);
vfs_node_t *vfs_file_node(vfs_file_t *file);

#endif /* _LYR_FS_VFS_H */
