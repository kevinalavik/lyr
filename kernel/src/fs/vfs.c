#include <fs/vfs.h>
#include <debug/log.h>
#include <mm/heap.h>
#include <mm/vmm.h>
#include <lib/string.h>

const vfs_cred_t vfs_root_cred = { .uid = 0, .gid = 0, .umask = 0022 };

typedef struct vfs_mount {
	vfs_node_t *covered;
	vfs_node_t *root;
	char path[512];
	struct vfs_mount *next;
} vfs_mount_t;

static vfs_node_t *root_node = NULL;
static vfs_mount_t *mounts = NULL;

static const vfs_cred_t *_cred_or_root(const vfs_cred_t *cred)
{
	return cred ? cred : &vfs_root_cred;
}

static int _is_group_member(const vfs_cred_t *cred, vfs_gid_t gid)
{
	if (cred->gid == gid)
		return 1;
	for (size_t i = 0; i < cred->group_count && i < VFS_SUPP_GROUP_MAX; i++) {
		if (cred->groups[i] == gid)
			return 1;
	}
	return 0;
}

static int _copy_name(const char *start, size_t len, char out[VFS_NAME_MAX + 1])
{
	if (len == 0)
		return -EINVAL;
	if (len > VFS_NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(out, start, len);
	out[len] = '\0';
	return 0;
}

static const char *_skip_slashes(const char *p)
{
	while (*p == '/')
		p++;
	return p;
}

void vfs_node_init(vfs_node_t *node, const vfs_ops_t *ops, vfs_mode_t mode,
				   vfs_uid_t uid, vfs_gid_t gid)
{
	memset(node, 0, sizeof(*node));
	node->ops = ops;
	node->dev = 0;
	node->ino = (uint64_t)(uintptr_t)node;
	node->mode = mode;
	node->uid = uid;
	node->gid = gid;
	node->nlink = VFS_S_ISDIR(mode) ? 2 : 1;
	node->refs = 1;
}

void vfs_node_ref(vfs_node_t *node)
{
	if (node)
		node->refs++;
}

void vfs_node_release(vfs_node_t *node)
{
	if (!node || node->refs == 0)
		return;
	node->refs--;
	if (node->refs == 0 && node->ops && node->ops->release)
		node->ops->release(node);
}

void vfs_init(vfs_node_t *root)
{
	root_node = root;
	log_debug("vfs", "root initialized node=%p mode=0%o", root,
			  root ? root->mode : 0);
}

vfs_node_t *vfs_root(void)
{
	return root_node;
}

int vfs_chroot(const char *path, const vfs_cred_t *cred)
{
	vfs_node_t *new_root = NULL;
	int r = vfs_resolve(path, cred, &new_root);
	if (r != 0)
		return r;
	if (!VFS_S_ISDIR(new_root->mode)) {
		vfs_node_release(new_root);
		return -ENOTDIR;
	}

	root_node = new_root;
	log_debug("vfs", "changed root to %s node=%p", path, new_root);
	return 0;
}

int vfs_change_root(const char *path, const vfs_cred_t *cred)
{
	vfs_node_t *new_root = NULL;
	int r = vfs_resolve(path, cred, &new_root);
	if (r != 0)
		return r;
	if (!VFS_S_ISDIR(new_root->mode)) {
		vfs_node_release(new_root);
		return -ENOTDIR;
	}

	root_node = new_root;

	vfs_mount_t *mnt = mounts;
	mounts = NULL;
	while (mnt) {
		vfs_mount_t *next = mnt->next;
		vfs_node_release(mnt->covered);
		vfs_node_release(mnt->root);
		kfree(mnt);
		mnt = next;
	}

	log_debug("vfs", "changed root to %s node=%p", path, new_root);
	return 0;
}

static int _path_eq(const char *a, const char *b)
{
	return a && b && strcmp(a, b) == 0;
}

static vfs_node_t *_mounted_root(vfs_node_t *node, const char *path)
{
	for (vfs_mount_t *mnt = mounts; mnt; mnt = mnt->next) {
		if (_path_eq(mnt->path, path))
			return mnt->root;
	}
	for (vfs_mount_t *mnt = mounts; mnt; mnt = mnt->next) {
		if (mnt->covered == node)
			return mnt->root;
	}
	return NULL;
}

static vfs_node_t *_follow_mount_at(vfs_node_t *node, const char *path)
{
	vfs_node_t *mounted = _mounted_root(node, path);
	if (!mounted)
		return node;
	vfs_node_ref(mounted);
	vfs_node_release(node);
	return mounted;
}

static int _append_component(char *path, size_t cap, const char *name,
							 size_t len)
{
	if (!path || !name || cap == 0)
		return -EINVAL;
	size_t cur = strlen(path);
	if (cur == 0)
		return -EINVAL;
	if (!(cur == 1 && path[0] == '/')) {
		if (cur + 1 >= cap)
			return -ENAMETOOLONG;
		path[cur++] = '/';
		path[cur] = '\0';
	}
	if (cur + len >= cap)
		return -ENAMETOOLONG;
	memcpy(path + cur, name, len);
	path[cur + len] = '\0';
	return 0;
}

int vfs_mount(const char *path, vfs_node_t *root, const vfs_cred_t *cred)
{
	if (!path || !root)
		return -EINVAL;

	vfs_node_t *covered = NULL;
	int r = vfs_resolve(path, cred, &covered);
	if (r != 0)
		return r;
	if (!VFS_S_ISDIR(covered->mode) || !VFS_S_ISDIR(root->mode)) {
		vfs_node_release(covered);
		return -ENOTDIR;
	}

	for (vfs_mount_t *mnt = mounts; mnt; mnt = mnt->next) {
		if (mnt->covered == covered) {
			vfs_node_release(covered);
			return -EEXIST;
		}
	}

	vfs_mount_t *mnt = kzalloc(sizeof(*mnt));
	if (!mnt) {
		vfs_node_release(covered);
		return -ENOMEM;
	}
	mnt->covered = covered;
	size_t mnt_path_len = strlen(path);
	if (mnt_path_len >= sizeof(mnt->path)) {
		vfs_node_release(covered);
		kfree(mnt);
		return -ENAMETOOLONG;
	}
	memcpy(mnt->path, path, mnt_path_len + 1);
	vfs_node_ref(root);
	mnt->root = root;
	mnt->next = mounts;
	mounts = mnt;
	log_debug("vfs", "mounted node=%p on %s covered=%p", root, path, covered);
	return 0;
}

int vfs_access(vfs_node_t *node, const vfs_cred_t *cred, int mask)
{
	cred = _cred_or_root(cred);
	if (!node)
		return -ENOENT;
	if ((mask & ~(VFS_R_OK | VFS_W_OK | VFS_X_OK)) != 0)
		return -EINVAL;

	if (cred->uid == 0) {
		if (mask & VFS_X_OK) {
			if (VFS_S_ISDIR(node->mode) ||
				(node->mode & (VFS_S_IXUSR | VFS_S_IXGRP | VFS_S_IXOTH)))
				return 0;
			return -EACCES;
		}
		return 0;
	}

	vfs_mode_t bits;
	if (cred->uid == node->uid)
		bits = (node->mode >> 6) & 7;
	else if (_is_group_member(cred, node->gid))
		bits = (node->mode >> 3) & 7;
	else
		bits = node->mode & 7;

	if ((mask & VFS_R_OK) && !(bits & 4))
		return -EACCES;
	if ((mask & VFS_W_OK) && !(bits & 2))
		return -EACCES;
	if ((mask & VFS_X_OK) && !(bits & 1))
		return -EACCES;
	return 0;
}

int vfs_resolve(const char *path, const vfs_cred_t *cred, vfs_node_t **out)
{
	cred = _cred_or_root(cred);
	log_trace("vfs", "resolve path=%s uid=%u", path ? path : "(null)",
			  cred->uid);
	if (!root_node || !path || !out)
		return -EINVAL;
	if (*path != '/')
		return -EINVAL;

	vfs_node_t *cur = root_node;
	vfs_node_ref(cur);
	char cur_path[512];
	cur_path[0] = '/';
	cur_path[1] = '\0';
	cur = _follow_mount_at(cur, cur_path);

	const char *p = _skip_slashes(path);
	if (*p == '\0') {
		*out = cur;
		return 0;
	}

	while (*p) {
		if (!VFS_S_ISDIR(cur->mode)) {
			vfs_node_release(cur);
			return -ENOTDIR;
		}
		int r = vfs_access(cur, cred, VFS_X_OK);
		if (r != 0) {
			vfs_node_release(cur);
			return r;
		}

		const char *start = p;
		while (*p && *p != '/')
			p++;
		size_t len = (size_t)(p - start);

		vfs_node_t *next = NULL;
		if (!cur->ops || !cur->ops->lookup) {
			vfs_node_release(cur);
			return -ENOTDIR;
		}
		r = cur->ops->lookup(cur, start, len, &next);
		vfs_node_release(cur);
		if (r != 0)
			return r;

		int pr = _append_component(cur_path, sizeof(cur_path), start, len);
		if (pr != 0) {
			vfs_node_release(next);
			return pr;
		}
		cur = next;
		cur = _follow_mount_at(cur, cur_path);
		p = _skip_slashes(p);
	}

	*out = cur;
	return 0;
}

static int _resolve_parent(const char *path, const vfs_cred_t *cred,
						   vfs_node_t **parent, char name[VFS_NAME_MAX + 1],
						   size_t *name_len)
{
	if (!path || *path != '/')
		return -EINVAL;

	const char *end = path + strlen(path);
	while (end > path + 1 && end[-1] == '/')
		end--;

	const char *slash = end;
	while (slash > path && slash[-1] != '/')
		slash--;

	size_t len = (size_t)(end - slash);
	int r = _copy_name(slash, len, name);
	if (r != 0)
		return r;
	if (name_len)
		*name_len = len;

	if (slash == path + 1) {
		*parent = root_node;
		vfs_node_ref(*parent);
		return 0;
	}

	size_t parent_len = (size_t)(slash - path - 1);
	char *tmp = kmalloc(parent_len + 2);
	if (!tmp)
		return -ENOMEM;
	memcpy(tmp, path, parent_len + 1);
	tmp[parent_len + 1] = '\0';

	r = vfs_resolve(tmp, cred, parent);
	kfree(tmp);
	return r;
}

int vfs_open(const char *path, uint32_t flags, vfs_mode_t mode,
			 const vfs_cred_t *cred, vfs_file_t **out)
{
	cred = _cred_or_root(cred);
	log_debug("vfs", "open path=%s flags=0x%x mode=0%o uid=%u",
			  path ? path : "(null)", flags, mode, cred->uid);
	if (!out)
		return -EINVAL;

	vfs_node_t *node = NULL;
	int r = vfs_resolve(path, cred, &node);
	log_debug("vfs", "open: %s -> node=%p ino=%u mode=0o%o",
			  path, node, node ? (unsigned)node->ino : 0, node ? node->mode : 0);
	if (r == 0 && VFS_S_ISLNK(node->mode)) {
		char link_target[256];
		size_t link_len = sizeof(link_target) - 1;
		r = vfs_readlink_node(node, link_target, &link_len);
		uint32_t orig_ino = node->ino;
		vfs_node_release(node);
		if (r != 0) {
			log_debug("vfs", "open: readlink failed: %d", r);
			return r;
		}
		link_target[link_len] = '\0';
		log_debug("vfs", "open: symlink %s (ino=%u) -> %s", path, orig_ino, link_target);

		char abs_target[512];
		if (link_target[0] == '/') {
			strcpy(abs_target, link_target);
		} else {
			strcpy(abs_target, path);
			char *last_slash = strrchr(abs_target, '/');
			if (last_slash) {
				last_slash[1] = '\0';
				size_t dlen = strlen(abs_target);
				size_t tlen = strlen(link_target);
				if (dlen + tlen < sizeof(abs_target))
					strcat(abs_target, link_target);
				else
					return -ENAMETOOLONG;
			}
		}
		r = vfs_resolve(abs_target, cred, &node);
		log_debug("vfs", "open: resolved %s -> ino=%u mode=0o%o",
				  abs_target, node ? (unsigned)node->ino : 0, node ? node->mode : 0);
		if (r != 0)
			return r;
	}
	if (r != 0 && (flags & VFS_O_CREAT)) {
		if (r != -ENOENT)
			return r;
		vfs_node_t *parent = NULL;
		char name[VFS_NAME_MAX + 1];
		size_t name_len = 0;
		r = _resolve_parent(path, cred, &parent, name, &name_len);
		if (r != 0)
			return r;
		if (!VFS_S_ISDIR(parent->mode)) {
			vfs_node_release(parent);
			return -ENOTDIR;
		}
		r = vfs_access(parent, cred, VFS_W_OK | VFS_X_OK);
		if (r == 0) {
			vfs_mode_t create_mode = (mode & VFS_S_PERM) & ~cred->umask;
			create_mode |= VFS_S_IFREG;
			if (parent->ops && parent->ops->create)
				r = parent->ops->create(parent, name, name_len, create_mode,
										cred, &node);
			else
				r = -ENOSYS;
		}
		vfs_node_release(parent);
		if (r != 0)
			return r;
	} else if (r == 0 && (flags & (VFS_O_CREAT | VFS_O_EXCL)) ==
							 (VFS_O_CREAT | VFS_O_EXCL)) {
		vfs_node_release(node);
		return -EEXIST;
	} else if (r != 0) {
		return r;
	}

	if ((flags & VFS_O_DIRECTORY) && !VFS_S_ISDIR(node->mode)) {
		vfs_node_release(node);
		return -ENOTDIR;
	}

	int acc = 0;
	switch (flags & VFS_O_ACCMODE) {
	case VFS_O_RDONLY:
		acc = VFS_R_OK;
		break;
	case VFS_O_WRONLY:
		acc = VFS_W_OK;
		break;
	case VFS_O_RDWR:
		acc = VFS_R_OK | VFS_W_OK;
		break;
	default:
		vfs_node_release(node);
		return -EINVAL;
	}
	if ((acc & VFS_W_OK) && VFS_S_ISDIR(node->mode)) {
		vfs_node_release(node);
		return -EISDIR;
	}
	r = vfs_access(node, cred, acc);
	if (r != 0) {
		vfs_node_release(node);
		return r;
	}

	if ((flags & VFS_O_TRUNC) && (acc & VFS_W_OK)) {
		if (!node->ops || !node->ops->truncate) {
			vfs_node_release(node);
			return -ENOSYS;
		}
		r = node->ops->truncate(node, 0);
		if (r != 0) {
			vfs_node_release(node);
			return r;
		}
	}

	vfs_file_t *file = kzalloc(sizeof(*file));
	if (!file) {
		vfs_node_release(node);
		return -ENOMEM;
	}
	file->node = node;
	file->flags = flags;
	file->offset = (flags & VFS_O_APPEND) ? node->size : 0;
	file->cred = *cred;

	if (node->ops && node->ops->open) {
		r = node->ops->open(file);
		if (r != 0) {
			vfs_node_release(node);
			kfree(file);
			return r;
		}
	}

	*out = file;
	log_trace("vfs", "open ok path=%s node=%p size=%llu", path, node,
			  node->size);
	return 0;
}

int vfs_close(vfs_file_t *file)
{
	if (!file)
		return -EBADF;

	int r = 0;
	if (file->node && file->node->ops && file->node->ops->close)
		r = file->node->ops->close(file);

	vfs_node_release(file->node);
	kfree(file);
	return r;
}

int vfs_read(vfs_file_t *file, void *buf, size_t len, size_t *done)
{
	if (done)
		*done = 0;
	if (!file || !buf)
		return -EBADF;
	if ((file->flags & VFS_O_ACCMODE) == VFS_O_WRONLY)
		return -EBADF;
	if (VFS_S_ISDIR(file->node->mode))
		return -EISDIR;
	if (!file->node->ops || !file->node->ops->read)
		return -ENOSYS;

	size_t n = 0;
	int r = file->node->ops->read(file->node, file->offset, buf, len, &n);
	if (r == 0)
		file->offset += n;
	if (done)
		*done = n;
	log_trace("vfs", "read node=%p len=%zu done=%zu status=%s(%d)", file->node,
			  len, n, errno_name(r), r);
	return r;
}

int vfs_write(vfs_file_t *file, const void *buf, size_t len, size_t *done)
{
	if (done)
		*done = 0;
	if (!file || !buf)
		return -EBADF;
	if ((file->flags & VFS_O_ACCMODE) == VFS_O_RDONLY)
		return -EBADF;
	if (VFS_S_ISDIR(file->node->mode))
		return -EISDIR;
	if (!file->node->ops || !file->node->ops->write)
		return -ENOSYS;

	uint64_t off =
		(file->flags & VFS_O_APPEND) ? file->node->size : file->offset;
	size_t n = 0;
	int r = file->node->ops->write(file->node, off, buf, len, &n);
	if (r == 0)
		file->offset = off + n;
	if (done)
		*done = n;
	log_trace("vfs", "write node=%p off=%llu len=%zu done=%zu status=%s(%d)",
			  file->node, off, len, n, errno_name(r), r);
	return r;
}

int vfs_ioctl(vfs_file_t *file, unsigned long request, void *arg)
{
	if (!file || !file->node)
		return -EBADF;
	if (!file->node->ops || !file->node->ops->ioctl)
		return -ENOTTY;

	int r = file->node->ops->ioctl(file, request, arg);
	log_trace("vfs", "ioctl node=%p req=0x%lx status=%s(%d)", file->node,
			  request, errno_name(r), r);
	return r;
}

int vfs_poll(vfs_file_t *file, int events)
{
	if (!file || !file->node)
		return -EBADF;
	if (!file->node->ops || !file->node->ops->poll)
		return 0;

	int r = file->node->ops->poll(file, events);
	log_trace("vfs", "poll node=%p events=0x%x revents=0x%x", file->node,
			  events, r);
	return r;
}

int vfs_readdir(vfs_node_t *dir, size_t index, vfs_dirent_t *out)
{
	if (!dir || !out)
		return -EINVAL;
	if (!VFS_S_ISDIR(dir->mode))
		return -ENOTDIR;
	if (!dir->ops || !dir->ops->readdir)
		return -ENOSYS;
	memset(out, 0, sizeof(*out));
	return dir->ops->readdir(dir, index, out);
}

int vfs_seek(vfs_file_t *file, int whence, int64_t off, uint64_t *new_off)
{
	if (!file)
		return -EBADF;

	uint64_t base;
	switch (whence) {
	case VFS_SEEK_SET:
		base = 0;
		break;
	case VFS_SEEK_CUR:
		base = file->offset;
		break;
	case VFS_SEEK_END:
		base = file->node->size;
		break;
	default:
		return -EINVAL;
	}

	if (off < 0 && (uint64_t)(-off) > base)
		return -EINVAL;
	file->offset = off < 0 ? base - (uint64_t)(-off) : base + (uint64_t)off;
	if (new_off)
		*new_off = file->offset;
	return 0;
}

int vfs_mkdir(const char *path, vfs_mode_t mode, const vfs_cred_t *cred)
{
	cred = _cred_or_root(cred);
	log_debug("vfs", "mkdir path=%s mode=0%o uid=%u", path ? path : "(null)",
			  mode, cred->uid);
	vfs_node_t *parent = NULL;
	char name[VFS_NAME_MAX + 1];
	size_t name_len = 0;
	int r = _resolve_parent(path, cred, &parent, name, &name_len);
	if (r != 0)
		return r;
	r = vfs_access(parent, cred, VFS_W_OK | VFS_X_OK);
	if (r == 0) {
		vfs_mode_t create_mode =
			VFS_S_IFDIR | ((mode & VFS_S_PERM) & ~cred->umask);
		if (parent->ops && parent->ops->mkdir) {
			vfs_node_t *created = NULL;
			r = parent->ops->mkdir(parent, name, name_len, create_mode, cred,
								   &created);
			vfs_node_release(created);
		} else {
			r = -ENOSYS;
		}
	}
	vfs_node_release(parent);
	return r;
}

static int _unlink_common(const char *path, const vfs_cred_t *cred, int dir)
{
	cred = _cred_or_root(cred);
	vfs_node_t *parent = NULL;
	char name[VFS_NAME_MAX + 1];
	size_t name_len = 0;
	int r = _resolve_parent(path, cred, &parent, name, &name_len);
	if (r != 0)
		return r;

	r = vfs_access(parent, cred, VFS_W_OK | VFS_X_OK);
	vfs_node_t *victim = NULL;
	if (r == 0 && parent->ops && parent->ops->lookup)
		r = parent->ops->lookup(parent, name, name_len, &victim);
	if (r == 0 && (parent->mode & VFS_S_ISVTX) && cred->uid != 0 &&
		cred->uid != parent->uid && cred->uid != victim->uid)
		r = -EPERM;
	if (r == 0 && dir != VFS_S_ISDIR(victim->mode))
		r = dir ? -ENOTDIR : -EISDIR;
	if (r == 0) {
		if (dir && parent->ops && parent->ops->rmdir)
			r = parent->ops->rmdir(parent, name, name_len);
		else if (!dir && parent->ops && parent->ops->unlink)
			r = parent->ops->unlink(parent, name, name_len);
		else
			r = -ENOSYS;
	}
	vfs_node_release(victim);
	vfs_node_release(parent);
	return r;
}

int vfs_unlink(const char *path, const vfs_cred_t *cred)
{
	log_debug("vfs", "unlink path=%s", path ? path : "(null)");
	return _unlink_common(path, cred, 0);
}

int vfs_rmdir(const char *path, const vfs_cred_t *cred)
{
	log_debug("vfs", "rmdir path=%s", path ? path : "(null)");
	return _unlink_common(path, cred, 1);
}

int vfs_rename(const char *old_path, const char *new_path,
			   const vfs_cred_t *cred)
{
	cred = _cred_or_root(cred);
	log_debug("vfs", "rename old=%s new=%s uid=%u",
			  old_path ? old_path : "(null)", new_path ? new_path : "(null)",
			  cred->uid);
	if (!old_path || !new_path)
		return -EINVAL;
	if (!strcmp(old_path, new_path))
		return 0;

	vfs_node_t *old_parent = NULL;
	vfs_node_t *new_parent = NULL;
	vfs_node_t *source = NULL;
	vfs_node_t *target = NULL;
	char old_name[VFS_NAME_MAX + 1];
	char new_name[VFS_NAME_MAX + 1];
	size_t old_len = 0;
	size_t new_len = 0;

	int r = _resolve_parent(old_path, cred, &old_parent, old_name, &old_len);
	if (r != 0)
		return r;

	r = _resolve_parent(new_path, cred, &new_parent, new_name, &new_len);
	if (r != 0) {
		vfs_node_release(old_parent);
		return r;
	}

	r = vfs_access(old_parent, cred, VFS_W_OK | VFS_X_OK);
	if (r == 0)
		r = vfs_access(new_parent, cred, VFS_W_OK | VFS_X_OK);
	if (r != 0)
		goto done;

	if (!old_parent->ops || !old_parent->ops->lookup) {
		r = -ENOSYS;
		goto done;
	}

	r = old_parent->ops->lookup(old_parent, old_name, old_len, &source);
	if (r != 0)
		goto done;

	if ((old_parent->mode & VFS_S_ISVTX) && cred->uid != 0 &&
		cred->uid != old_parent->uid && cred->uid != source->uid) {
		r = -EPERM;
		goto done;
	}

	if (old_parent->ops != new_parent->ops) {
		r = -EXDEV;
		goto done;
	}

	if (!old_parent->ops->rename) {
		r = -ENOSYS;
		goto done;
	}

	if (new_parent->ops && new_parent->ops->lookup) {
		r = new_parent->ops->lookup(new_parent, new_name, new_len, &target);
		if (r == 0) {
			if (target == source) {
				r = 0;
				goto done;
			}
			if ((new_parent->mode & VFS_S_ISVTX) && cred->uid != 0 &&
				cred->uid != new_parent->uid && cred->uid != target->uid) {
				r = -EPERM;
				goto done;
			}
		} else if (r != -ENOENT) {
			goto done;
		}
	}

	r = old_parent->ops->rename(old_parent, old_name, old_len, new_parent,
								new_name, new_len);

done:
	vfs_node_release(target);
	vfs_node_release(source);
	vfs_node_release(new_parent);
	vfs_node_release(old_parent);
	return r;
}

int vfs_chmod(const char *path, vfs_mode_t mode, const vfs_cred_t *cred)
{
	cred = _cred_or_root(cred);
	vfs_node_t *node = NULL;
	int r = vfs_resolve(path, cred, &node);
	if (r != 0)
		return r;
	if (cred->uid != 0 && cred->uid != node->uid) {
		vfs_node_release(node);
		return -EPERM;
	}
	node->mode = (node->mode & VFS_S_IFMT) | (mode & VFS_S_PERM);
	log_trace("vfs", "chmod path=%s mode=0%o", path, node->mode);
	vfs_node_release(node);
	return 0;
}

int vfs_chown(const char *path, vfs_uid_t uid, vfs_gid_t gid,
			  const vfs_cred_t *cred)
{
	cred = _cred_or_root(cred);
	if (cred->uid != 0)
		return -EPERM;
	vfs_node_t *node = NULL;
	int r = vfs_resolve(path, cred, &node);
	if (r != 0)
		return r;
	node->uid = uid;
	node->gid = gid;
	node->mode &= ~(VFS_S_ISUID | VFS_S_ISGID);
	vfs_node_release(node);
	return 0;
}

int vfs_stat(const char *path, const vfs_cred_t *cred, vfs_stat_t *st)
{
	if (!st)
		return -EINVAL;
	vfs_node_t *node = NULL;
	int r = vfs_resolve(path, cred, &node);
	if (r != 0)
		return r;
	memset(st, 0, sizeof(*st));
	st->dev = node->dev;
	st->ino = node->ino;
	st->mode = node->mode;
	st->uid = node->uid;
	st->gid = node->gid;
	st->size = node->size;
	st->nlink = node->nlink;
	st->blksize = 4096;
	st->blocks = (node->size + 511) / 512;
	vfs_node_release(node);
	return 0;
}

int vfs_readlink_node(vfs_node_t *node, char *buf, size_t *size)
{
	if (!node || !buf || !size || *size == 0)
		return -EINVAL;

	if (!VFS_S_ISLNK(node->mode))
		return -EINVAL;

	size_t read_size = node->size;
	if (read_size > *size)
		read_size = *size;

	if (!node->ops || !node->ops->read) {
		vfs_node_release(node);
		return -ENOSYS;
	}

	size_t done = 0;
	int r = node->ops->read(node, 0, buf, read_size, &done);
	if (r != 0)
		return r;

	*size = done;
	return 0;
}

int vfs_readlink(const char *path, const vfs_cred_t *cred, char *buf,
				 size_t size)
{
	if (!buf || size == 0)
		return -EINVAL;

	vfs_node_t *node = NULL;
	int r = vfs_resolve(path, cred, &node);
	if (r != 0)
		return r;

	if (!VFS_S_ISLNK(node->mode)) {
		vfs_node_release(node);
		return -EINVAL;
	}

	if (size > node->size)
		size = node->size;

	if (!node->ops || !node->ops->read) {
		vfs_node_release(node);
		return -ENOSYS;
	}

	size_t done = 0;
	r = node->ops->read(node, 0, buf, size, &done);
	vfs_node_release(node);

	if (r != 0)
		return r;

	return (int)done;
}

int vfs_node_get_page(vfs_node_t *node, uint64_t page_index, int for_write,
					  page_t **out)
{
	if (!node || !out)
		return -EINVAL;
	if (!node->ops || !node->ops->get_page)
		return -ENOSYS;
	return node->ops->get_page(node, page_index, for_write, out);
}

uint64_t vfs_mmap(vfs_file_t *file, struct vas *vas, uint64_t hint,
				  uint64_t offset, size_t length, uint64_t flags)
{
	if (!file || !file->node || !vas || !length)
		return 0;
	if (VFS_S_ISDIR(file->node->mode))
		return 0;
	if (file->node->ops && file->node->ops->mmap)
		return file->node->ops->mmap(file, vas, hint, offset, length, flags);
	return vas_map_file(vas, hint, file->node, offset, length, flags);
}

vfs_node_t *vfs_file_node(vfs_file_t *file)
{
	return file ? file->node : NULL;
}
