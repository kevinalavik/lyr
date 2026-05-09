#include <fs/mount.h>
#include <dev/block.h>
#include <fs/ext2.h>
#include <fs/devfs.h>
#include <fs/tmpfs.h>
#include <fs/procfs.h>
#include <fs/vfs.h>
#include <lib/string.h>

static const char *block_name_from_source(const char *source)
{
	static const char prefix[] = "/dev/";
	size_t prefix_len = sizeof(prefix) - 1;

	if (!source)
		return NULL;
	if (strncmp(source, prefix, prefix_len) == 0)
		return source + prefix_len;
	return source;
}

static int ensure_mountpoint(const char *target)
{
	vfs_node_t *node = NULL;
	int r = vfs_resolve(target, &vfs_root_cred, &node);
	if (r == VFS_OK) {
		int ok = VFS_S_ISDIR(node->mode) ? VFS_OK : VFS_ERR_NOTDIR;
		vfs_node_release(node);
		return ok;
	}
	if (r != VFS_ERR_NOENT)
		return r;
	return vfs_mkdir(target, 0755, &vfs_root_cred);
}

int fs_mount_spec(const char *source, const char *target, const char *fstype,
				  uint64_t flags, const char *data)
{
	(void)flags;
	(void)data;

	if (!target || !fstype)
		return VFS_ERR_INVAL;

	if (strcmp(fstype, "devfs") == 0) {
		int r = ensure_mountpoint(target);
		if (r != VFS_OK)
			return r;
		return devfs_mount(target);
	}

	if (strcmp(fstype, "tmpfs") == 0) {
		int r = ensure_mountpoint(target);
		if (r != VFS_OK)
			return r;
		vfs_node_t *root = tmpfs_create_root(0755, 0, 0);
		if (!root)
			return VFS_ERR_NOMEM;
		r = vfs_mount(target, root, &vfs_root_cred);
		vfs_node_release(root);
		return r;
	}

	if (strcmp(fstype, "procfs") == 0 || strcmp(fstype, "proc") == 0) {
		int r = ensure_mountpoint(target);
		if (r != VFS_OK)
			return r;
		return procfs_mount(target);
	}

	if (strcmp(fstype, "ext2") == 0) {
		if (!source)
			return VFS_ERR_INVAL;
		block_device_t *dev = block_find(block_name_from_source(source));
		if (!dev)
			return VFS_ERR_NOENT;
		return ext2_mount(dev, target);
	}

	return VFS_ERR_NOSYS;
}
