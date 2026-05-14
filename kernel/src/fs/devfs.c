#include <fs/devfs.h>
#include <fs/procfs.h>
#include <debug/log.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <util/kprintf.h>

typedef struct devfs_node {
	vfs_node_t vnode;
	struct devfs_node *parent;
	struct devfs_node *children;
	struct devfs_node *next;
	char name[VFS_NAME_MAX + 1];
	devfs_read_t read;
	devfs_write_t write;
	devfs_ioctl_t ioctl;
	devfs_poll_t poll;
	devfs_close_t close;
	devfs_mmap_t mmap;
	void *ctx;
} devfs_node_t;

static int devfs_lookup(vfs_node_t *dir, const char *name, size_t len,
						vfs_node_t **out);
static int devfs_mkdir_op(vfs_node_t *dir, const char *name, size_t len,
						  vfs_mode_t mode, const vfs_cred_t *cred,
						  vfs_node_t **out);
static int devfs_unlink_op(vfs_node_t *dir, const char *name, size_t len);
static int devfs_rmdir_op(vfs_node_t *dir, const char *name, size_t len);
static int devfs_read_op(vfs_node_t *node, uint64_t off, void *buf, size_t len,
						 size_t *done);
static int devfs_write_op(vfs_node_t *node, uint64_t off, const void *buf,
						  size_t len, size_t *done);
static int devfs_ioctl_op(vfs_file_t *file, unsigned long request, void *arg);
static int devfs_poll_op(vfs_file_t *file, int events);
static int devfs_close_op(vfs_file_t *file);
static uint64_t devfs_mmap_op(vfs_file_t *file, struct vas *vas, uint64_t hint,
							  uint64_t offset, size_t length, uint64_t flags);
static int devfs_readdir(vfs_node_t *dir, size_t index, vfs_dirent_t *out);
static void devfs_release(vfs_node_t *node);

static const vfs_ops_t devfs_ops = {
	.lookup = devfs_lookup,
	.mkdir = devfs_mkdir_op,
	.unlink = devfs_unlink_op,
	.rmdir = devfs_rmdir_op,
	.read = devfs_read_op,
	.write = devfs_write_op,
	.ioctl = devfs_ioctl_op,
	.poll = devfs_poll_op,
	.close = devfs_close_op,
	.mmap = devfs_mmap_op,
	.readdir = devfs_readdir,
	.release = devfs_release,
};

static devfs_node_t *devfs_root_node = NULL;

static int null_read(void *ctx, uint64_t off, void *buf, size_t len,
					 size_t *done)
{
	(void)ctx;
	(void)off;
	(void)buf;
	(void)len;
	if (done)
		*done = 0;
	return 0;
}

static int null_write(void *ctx, uint64_t off, const void *buf, size_t len,
					  size_t *done)
{
	(void)ctx;
	(void)off;
	(void)buf;
	if (done)
		*done = len;
	return 0;
}

static int zero_read(void *ctx, uint64_t off, void *buf, size_t len,
					 size_t *done)
{
	(void)ctx;
	(void)off;
	memset(buf, 0, len);
	if (done)
		*done = len;
	return 0;
}

static int kmsg_write(void *ctx, uint64_t off, const void *buf, size_t len,
					  size_t *done)
{
	(void)ctx;
	(void)off;
	if (!buf)
		return -EINVAL;
	char line[192];
	size_t n = len;
	if (n >= sizeof(line))
		n = sizeof(line) - 1;
	memcpy(line, buf, n);
	line[n] = '\0';
	kprintf("%s", line);
	if (done)
		*done = len;
	return 0;
}

static devfs_node_t *to_devfs(vfs_node_t *node)
{
	return (devfs_node_t *)node;
}

static int name_eq(devfs_node_t *node, const char *name, size_t len)
{
	return strlen(node->name) == len && memcmp(node->name, name, len) == 0;
}

static devfs_node_t *find_child(devfs_node_t *dir, const char *name, size_t len)
{
	for (devfs_node_t *child = dir->children; child; child = child->next) {
		if (name_eq(child, name, len))
			return child;
	}
	return NULL;
}

static devfs_node_t *alloc_node(const char *name, size_t len, vfs_mode_t mode)
{
	if (len > VFS_NAME_MAX)
		return NULL;
	devfs_node_t *node = kzalloc(sizeof(*node));
	if (!node)
		return NULL;
	vfs_node_init(&node->vnode, &devfs_ops, mode, 0, 0);
	memcpy(node->name, name, len);
	node->name[len] = '\0';
	node->vnode.private_data = node;
	return node;
}

static void insert_child(devfs_node_t *dir, devfs_node_t *child)
{
	child->parent = dir;
	child->next = dir->children;
	dir->children = child;
	if (VFS_S_ISDIR(child->vnode.mode))
		dir->vnode.nlink++;
}

static void unlink_child(devfs_node_t *dir, devfs_node_t *child)
{
	devfs_node_t **cur = &dir->children;
	while (*cur) {
		if (*cur == child) {
			*cur = child->next;
			child->next = NULL;
			child->parent = NULL;
			if (VFS_S_ISDIR(child->vnode.mode) && dir->vnode.nlink > 0)
				dir->vnode.nlink--;
			return;
		}
		cur = &(*cur)->next;
	}
}

static int create_child(devfs_node_t *dir, const char *name, size_t len,
						vfs_mode_t mode, devfs_node_t **out)
{
	if (!VFS_S_ISDIR(dir->vnode.mode))
		return -ENOTDIR;
	if (len == 0 || len > VFS_NAME_MAX)
		return len == 0 ? -EINVAL : -ENAMETOOLONG;
	if (find_child(dir, name, len))
		return -EEXIST;

	devfs_node_t *child = alloc_node(name, len, mode);
	if (!child)
		return -ENOMEM;
	insert_child(dir, child);
	if (out)
		*out = child;
	return 0;
}

static int remove_child(devfs_node_t *dir, const char *name, size_t len,
						int want_dir)
{
	devfs_node_t *child = find_child(dir, name, len);
	if (!child)
		return -ENOENT;
	if (want_dir && !VFS_S_ISDIR(child->vnode.mode))
		return -ENOTDIR;
	if (!want_dir && VFS_S_ISDIR(child->vnode.mode))
		return -EISDIR;
	if (want_dir && child->children)
		return -ENOTEMPTY;
	unlink_child(dir, child);
	child->vnode.nlink = 0;
	vfs_node_release(&child->vnode);
	return 0;
}

static const char *skip_dev_prefix(const char *path)
{
	if (!path)
		return NULL;
	if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' &&
		(path[4] == '/' || path[4] == '\0'))
		return path + 4;
	return path;
}

static const char *skip_slashes(const char *p)
{
	while (*p == '/')
		p++;
	return p;
}

static int resolve_parent(const char *path, devfs_node_t **parent,
						  const char **name, size_t *name_len, int make_dirs)
{
	if (!devfs_root_node || !path || !parent || !name || !name_len)
		return -EINVAL;

	const char *p = skip_slashes(skip_dev_prefix(path));
	devfs_node_t *cur = devfs_root_node;
	if (*p == '\0')
		return -EINVAL;

	for (;;) {
		const char *start = p;
		while (*p && *p != '/')
			p++;
		size_t len = (size_t)(p - start);
		const char *next = skip_slashes(p);
		if (*next == '\0') {
			*parent = cur;
			*name = start;
			*name_len = len;
			return 0;
		}

		devfs_node_t *child = find_child(cur, start, len);
		if (!child) {
			if (!make_dirs)
				return -ENOENT;
			int r = create_child(cur, start, len, VFS_S_IFDIR | 0755, &child);
			if (r != 0)
				return r;
		}
		if (!VFS_S_ISDIR(child->vnode.mode))
			return -ENOTDIR;
		cur = child;
		p = next;
	}
}

static int devfs_lookup(vfs_node_t *dir_node, const char *name, size_t len,
						vfs_node_t **out)
{
	if (!VFS_S_ISDIR(dir_node->mode))
		return -ENOTDIR;
	if (len == 1 && name[0] == '.') {
		vfs_node_ref(dir_node);
		*out = dir_node;
		return 0;
	}

	devfs_node_t *dir = to_devfs(dir_node);
	if (len == 2 && name[0] == '.' && name[1] == '.') {
		vfs_node_t *parent = &dir->parent->vnode;
		vfs_node_ref(parent);
		*out = parent;
		return 0;
	}

	devfs_node_t *child = find_child(dir, name, len);
	if (!child)
		return -ENOENT;
	vfs_node_ref(&child->vnode);
	*out = &child->vnode;
	return 0;
}

static int devfs_mkdir_op(vfs_node_t *dir, const char *name, size_t len,
						  vfs_mode_t mode, const vfs_cred_t *cred,
						  vfs_node_t **out)
{
	(void)cred;
	devfs_node_t *child = NULL;
	int r = create_child(to_devfs(dir), name, len,
						 VFS_S_IFDIR | (mode & VFS_S_PERM), &child);
	if (r != 0)
		return r;
	vfs_node_ref(&child->vnode);
	*out = &child->vnode;
	return 0;
}

static int devfs_unlink_op(vfs_node_t *dir, const char *name, size_t len)
{
	(void)dir;
	(void)name;
	(void)len;
	return -EPERM;
}

static int devfs_rmdir_op(vfs_node_t *dir, const char *name, size_t len)
{
	(void)dir;
	(void)name;
	(void)len;
	return -EPERM;
}

static int devfs_read_op(vfs_node_t *node, uint64_t off, void *buf, size_t len,
						 size_t *done)
{
	if (done)
		*done = 0;
	if (VFS_S_ISDIR(node->mode))
		return -EISDIR;
	devfs_node_t *dn = to_devfs(node);
	if (!dn->read)
		return -ENOSYS;
	return dn->read(dn->ctx, off, buf, len, done);
}

static int devfs_write_op(vfs_node_t *node, uint64_t off, const void *buf,
						  size_t len, size_t *done)
{
	if (done)
		*done = 0;
	if (VFS_S_ISDIR(node->mode))
		return -EISDIR;
	devfs_node_t *dn = to_devfs(node);
	if (!dn->write)
		return -ENOSYS;
	return dn->write(dn->ctx, off, buf, len, done);
}

static int devfs_ioctl_op(vfs_file_t *file, unsigned long request, void *arg)
{
	if (!file || !file->node)
		return -EBADF;
	if (VFS_S_ISDIR(file->node->mode))
		return -EISDIR;
	devfs_node_t *dn = to_devfs(file->node);
	if (!dn->ioctl)
		return -ENOTTY;
	return dn->ioctl(dn->ctx, request, arg);
}

static int devfs_poll_op(vfs_file_t *file, int events)
{
	if (!file || !file->node)
		return -EBADF;
	if (VFS_S_ISDIR(file->node->mode))
		return -EISDIR;
	devfs_node_t *dn = to_devfs(file->node);
	if (!dn->poll)
		return 0;
	return dn->poll(dn->ctx, events);
}


static int devfs_close_op(vfs_file_t *file)
{
	if (!file || !file->node)
		return -EBADF;
	if (VFS_S_ISDIR(file->node->mode))
		return 0;
	devfs_node_t *dn = to_devfs(file->node);
	if (!dn->close)
		return 0;
	return dn->close(dn->ctx);
}

static uint64_t devfs_mmap_op(vfs_file_t *file, struct vas *vas, uint64_t hint,
							  uint64_t offset, size_t length, uint64_t flags)
{
	if (!file || !file->node || !vas)
		return 0;
	if (VFS_S_ISDIR(file->node->mode))
		return 0;
	devfs_node_t *dn = to_devfs(file->node);
	if (!dn->mmap)
		return 0;
	return dn->mmap(dn->ctx, vas, hint, offset, length, flags);
}

static int devfs_readdir(vfs_node_t *dir_node, size_t index, vfs_dirent_t *out)
{
	if (!VFS_S_ISDIR(dir_node->mode))
		return -ENOTDIR;
	if (!out)
		return -EINVAL;

	devfs_node_t *dir = to_devfs(dir_node);
	size_t i = 0;
	for (devfs_node_t *child = dir->children; child; child = child->next) {
		if (i++ != index)
			continue;
		size_t len = strlen(child->name);
		if (len > VFS_NAME_MAX)
			len = VFS_NAME_MAX;
		memcpy(out->name, child->name, len);
		out->name[len] = '\0';
		out->mode = child->vnode.mode;
		out->uid = child->vnode.uid;
		out->gid = child->vnode.gid;
		out->size = child->vnode.size;
		out->nlink = child->vnode.nlink;
		return 0;
	}
	return -ENOENT;
}

static void devfs_release(vfs_node_t *node)
{
	devfs_node_t *dn = to_devfs(node);
	while (dn->children) {
		devfs_node_t *child = dn->children;
		dn->children = child->next;
		child->next = NULL;
		child->vnode.nlink = 0;
		vfs_node_release(&child->vnode);
	}
	kfree(dn);
}

int devfs_mount(const char *target)
{
	if (!devfs_root_node || !target)
		return -EINVAL;

	int r = vfs_mount(target, &devfs_root_node->vnode, &vfs_root_cred);
	if (r == 0) {
		(void)procfs_note_mount("dev", target, "devfs", "rw");
		log_debug("devfs", "mounted on %s", target);
	}
	return r;
}

int devfs_init(void)
{
	if (devfs_root_node)
		return 0;

	int r = vfs_mkdir("/dev", 0755, &vfs_root_cred);
	if (r != 0 && r != -EEXIST)
		return r;

	devfs_root_node = alloc_node("", 0, VFS_S_IFDIR | 0755);
	if (!devfs_root_node)
		return -ENOMEM;
	devfs_root_node->parent = devfs_root_node;

	r = devfs_mount("/dev");
	if (r != 0)
		return r;
	devfs_register_chr("/dev/null", 0666, null_read, null_write, NULL);
	devfs_register_chr("/dev/zero", 0666, zero_read, null_write, NULL);
	devfs_register_chr("/dev/kmsg", 0220, NULL, kmsg_write, NULL);
	return 0;
}

int devfs_mkdir(const char *path, vfs_mode_t mode)
{
	devfs_node_t *parent = NULL;
	const char *name = NULL;
	size_t name_len = 0;
	int r = resolve_parent(path, &parent, &name, &name_len, 1);
	if (r != 0)
		return r;
	devfs_node_t *existing = find_child(parent, name, name_len);
	if (existing)
		return VFS_S_ISDIR(existing->vnode.mode) ? 0 : -EEXIST;
	return create_child(parent, name, name_len,
						VFS_S_IFDIR | (mode & VFS_S_PERM), NULL);
}

int devfs_register_chr_poll_close(const char *path, vfs_mode_t mode,
								  devfs_read_t read, devfs_write_t write,
								  devfs_ioctl_t ioctl, devfs_poll_t poll,
								  devfs_close_t close, void *ctx)
{
	return devfs_register_chr_mmap_poll_close(path, mode, read, write, ioctl,
											  poll, close, NULL, ctx);
}

int devfs_register_chr_mmap_poll_close(const char *path, vfs_mode_t mode,
									   devfs_read_t read, devfs_write_t write,
									   devfs_ioctl_t ioctl, devfs_poll_t poll,
									   devfs_close_t close, devfs_mmap_t mmap,
									   void *ctx)
{
	devfs_node_t *parent = NULL;
	const char *name = NULL;
	size_t name_len = 0;
	int r = resolve_parent(path, &parent, &name, &name_len, 1);
	if (r != 0)
		return r;
	if (find_child(parent, name, name_len))
		return -EEXIST;

	devfs_node_t *node = NULL;
	r = create_child(parent, name, name_len, VFS_S_IFCHR | (mode & VFS_S_PERM),
					 &node);
	if (r != 0)
		return r;
	node->read = read;
	node->write = write;
	node->ioctl = ioctl;
	node->poll = poll;
	node->close = close;
	node->mmap = mmap;
	node->ctx = ctx;
	log_debug("devfs", "registered char %s", path);
	return 0;
}

int devfs_register_chr_poll(const char *path, vfs_mode_t mode, devfs_read_t read,
							devfs_write_t write, devfs_ioctl_t ioctl,
							devfs_poll_t poll, void *ctx)
{
	return devfs_register_chr_poll_close(path, mode, read, write, ioctl, poll,
									 NULL, ctx);
}

int devfs_register_chr_ex(const char *path, vfs_mode_t mode, devfs_read_t read,
					  devfs_write_t write, devfs_ioctl_t ioctl, void *ctx)
{
	return devfs_register_chr_poll(path, mode, read, write, ioctl, NULL, ctx);
}

int devfs_register_chr(const char *path, vfs_mode_t mode, devfs_read_t read,
					   devfs_write_t write, void *ctx)
{
	return devfs_register_chr_ex(path, mode, read, write, NULL, ctx);
}


int devfs_set_size(const char *path, uint64_t size)
{
	devfs_node_t *parent = NULL;
	const char *name = NULL;
	size_t name_len = 0;
	int r = resolve_parent(path, &parent, &name, &name_len, 0);
	if (r != 0)
		return r;

	devfs_node_t *node = find_child(parent, name, name_len);
	if (!node)
		return -ENOENT;

	node->vnode.size = size;
	return 0;
}

int devfs_unregister(const char *path)
{
	devfs_node_t *parent = NULL;
	const char *name = NULL;
	size_t name_len = 0;
	int r = resolve_parent(path, &parent, &name, &name_len, 0);
	if (r != 0)
		return r;
	return remove_child(parent, name, name_len, 0);
}
