#include <fs/vfs.h>
#include <debug/log.h>
#include <mm/heap.h>
#include <lib/string.h>

const vfs_cred_t vfs_root_cred = { .uid = 0, .gid = 0, .umask = 0022 };

typedef struct vfs_mount {
	vfs_node_t *covered;
	vfs_node_t *root;
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
		return VFS_ERR_INVAL;
	if (len > VFS_NAME_MAX)
		return VFS_ERR_NAMETOOLONG;
	memcpy(out, start, len);
	out[len] = '\0';
	return VFS_OK;
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
	log_info("vfs", "root initialized node=%p mode=0%o", root,
			 root ? root->mode : 0);
}

vfs_node_t *vfs_root(void)
{
	return root_node;
}

static vfs_node_t *_mounted_root(vfs_node_t *node)
{
	for (vfs_mount_t *mnt = mounts; mnt; mnt = mnt->next) {
		if (mnt->covered == node)
			return mnt->root;
	}
	return NULL;
}

static vfs_node_t *_follow_mount(vfs_node_t *node)
{
	vfs_node_t *mounted = _mounted_root(node);
	if (!mounted)
		return node;
	vfs_node_ref(mounted);
	vfs_node_release(node);
	return mounted;
}

int vfs_mount(const char *path, vfs_node_t *root, const vfs_cred_t *cred)
{
	if (!path || !root)
		return VFS_ERR_INVAL;

	vfs_node_t *covered = NULL;
	int r = vfs_resolve(path, cred, &covered);
	if (r != VFS_OK)
		return r;
	if (!VFS_S_ISDIR(covered->mode) || !VFS_S_ISDIR(root->mode)) {
		vfs_node_release(covered);
		return VFS_ERR_NOTDIR;
	}

	for (vfs_mount_t *mnt = mounts; mnt; mnt = mnt->next) {
		if (mnt->covered == covered) {
			vfs_node_release(covered);
			return VFS_ERR_EXIST;
		}
	}

	vfs_mount_t *mnt = kzalloc(sizeof(*mnt));
	if (!mnt) {
		vfs_node_release(covered);
		return VFS_ERR_NOMEM;
	}
	mnt->covered = covered;
	vfs_node_ref(root);
	mnt->root = root;
	mnt->next = mounts;
	mounts = mnt;
	log_info("vfs", "mounted node=%p on %s covered=%p", root, path, covered);
	return VFS_OK;
}

int vfs_access(vfs_node_t *node, const vfs_cred_t *cred, int mask)
{
	cred = _cred_or_root(cred);
	if (!node)
		return VFS_ERR_NOENT;
	if ((mask & ~(VFS_R_OK | VFS_W_OK | VFS_X_OK)) != 0)
		return VFS_ERR_INVAL;

	if (cred->uid == 0) {
		if (mask & VFS_X_OK) {
			if (VFS_S_ISDIR(node->mode) ||
				(node->mode & (VFS_S_IXUSR | VFS_S_IXGRP | VFS_S_IXOTH)))
				return VFS_OK;
			return VFS_ERR_ACCES;
		}
		return VFS_OK;
	}

	vfs_mode_t bits;
	if (cred->uid == node->uid)
		bits = (node->mode >> 6) & 7;
	else if (_is_group_member(cred, node->gid))
		bits = (node->mode >> 3) & 7;
	else
		bits = node->mode & 7;

	if ((mask & VFS_R_OK) && !(bits & 4))
		return VFS_ERR_ACCES;
	if ((mask & VFS_W_OK) && !(bits & 2))
		return VFS_ERR_ACCES;
	if ((mask & VFS_X_OK) && !(bits & 1))
		return VFS_ERR_ACCES;
	return VFS_OK;
}

int vfs_resolve(const char *path, const vfs_cred_t *cred, vfs_node_t **out)
{
	cred = _cred_or_root(cred);
	log_trace("vfs", "resolve path=%s uid=%u", path ? path : "(null)",
			  cred->uid);
	if (!root_node || !path || !out)
		return VFS_ERR_INVAL;
	if (*path != '/')
		return VFS_ERR_INVAL;

	vfs_node_t *cur = root_node;
	vfs_node_ref(cur);
	cur = _follow_mount(cur);

	const char *p = _skip_slashes(path);
	if (*p == '\0') {
		*out = cur;
		return VFS_OK;
	}

	while (*p) {
		if (!VFS_S_ISDIR(cur->mode)) {
			vfs_node_release(cur);
			return VFS_ERR_NOTDIR;
		}
		int r = vfs_access(cur, cred, VFS_X_OK);
		if (r != VFS_OK) {
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
			return VFS_ERR_NOTDIR;
		}
		r = cur->ops->lookup(cur, start, len, &next);
		vfs_node_release(cur);
		if (r != VFS_OK)
			return r;

		cur = next;
		cur = _follow_mount(cur);
		p = _skip_slashes(p);
	}

	*out = cur;
	return VFS_OK;
}

static int _resolve_parent(const char *path, const vfs_cred_t *cred,
						   vfs_node_t **parent, char name[VFS_NAME_MAX + 1],
						   size_t *name_len)
{
	if (!path || *path != '/')
		return VFS_ERR_INVAL;

	const char *end = path + strlen(path);
	while (end > path + 1 && end[-1] == '/')
		end--;

	const char *slash = end;
	while (slash > path && slash[-1] != '/')
		slash--;

	size_t len = (size_t)(end - slash);
	int r = _copy_name(slash, len, name);
	if (r != VFS_OK)
		return r;
	if (name_len)
		*name_len = len;

	if (slash == path + 1) {
		*parent = root_node;
		vfs_node_ref(*parent);
		return VFS_OK;
	}

	size_t parent_len = (size_t)(slash - path - 1);
	char *tmp = kmalloc(parent_len + 2);
	if (!tmp)
		return VFS_ERR_NOMEM;
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
		return VFS_ERR_INVAL;

	vfs_node_t *node = NULL;
	int r = vfs_resolve(path, cred, &node);
	if (r != VFS_OK && (flags & VFS_O_CREAT)) {
		if (r != VFS_ERR_NOENT)
			return r;
		vfs_node_t *parent = NULL;
		char name[VFS_NAME_MAX + 1];
		size_t name_len = 0;
		r = _resolve_parent(path, cred, &parent, name, &name_len);
		if (r != VFS_OK)
			return r;
		if (!VFS_S_ISDIR(parent->mode)) {
			vfs_node_release(parent);
			return VFS_ERR_NOTDIR;
		}
		r = vfs_access(parent, cred, VFS_W_OK | VFS_X_OK);
		if (r == VFS_OK) {
			vfs_mode_t create_mode = (mode & VFS_S_PERM) & ~cred->umask;
			create_mode |= VFS_S_IFREG;
			if (parent->ops && parent->ops->create)
				r = parent->ops->create(parent, name, name_len, create_mode,
										cred, &node);
			else
				r = VFS_ERR_NOSYS;
		}
		vfs_node_release(parent);
		if (r != VFS_OK)
			return r;
	} else if (r == VFS_OK && (flags & (VFS_O_CREAT | VFS_O_EXCL)) ==
								  (VFS_O_CREAT | VFS_O_EXCL)) {
		vfs_node_release(node);
		return VFS_ERR_EXIST;
	} else if (r != VFS_OK) {
		return r;
	}

	if ((flags & VFS_O_DIRECTORY) && !VFS_S_ISDIR(node->mode)) {
		vfs_node_release(node);
		return VFS_ERR_NOTDIR;
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
		return VFS_ERR_INVAL;
	}
	if ((acc & VFS_W_OK) && VFS_S_ISDIR(node->mode)) {
		vfs_node_release(node);
		return VFS_ERR_ISDIR;
	}
	r = vfs_access(node, cred, acc);
	if (r != VFS_OK) {
		vfs_node_release(node);
		return r;
	}

	if ((flags & VFS_O_TRUNC) && (acc & VFS_W_OK)) {
		if (!node->ops || !node->ops->truncate) {
			vfs_node_release(node);
			return VFS_ERR_NOSYS;
		}
		r = node->ops->truncate(node, 0);
		if (r != VFS_OK) {
			vfs_node_release(node);
			return r;
		}
	}

	vfs_file_t *file = kzalloc(sizeof(*file));
	if (!file) {
		vfs_node_release(node);
		return VFS_ERR_NOMEM;
	}
	file->node = node;
	file->flags = flags;
	file->offset = (flags & VFS_O_APPEND) ? node->size : 0;
	file->cred = *cred;
	*out = file;
	log_trace("vfs", "open ok path=%s node=%p size=%llu", path, node,
			  node->size);
	return VFS_OK;
}

int vfs_close(vfs_file_t *file)
{
	if (!file)
		return VFS_ERR_BADF;
	vfs_node_release(file->node);
	kfree(file);
	return VFS_OK;
}

int vfs_read(vfs_file_t *file, void *buf, size_t len, size_t *done)
{
	if (done)
		*done = 0;
	if (!file || !buf)
		return VFS_ERR_BADF;
	if ((file->flags & VFS_O_ACCMODE) == VFS_O_WRONLY)
		return VFS_ERR_BADF;
	if (VFS_S_ISDIR(file->node->mode))
		return VFS_ERR_ISDIR;
	if (!file->node->ops || !file->node->ops->read)
		return VFS_ERR_NOSYS;

	size_t n = 0;
	int r = file->node->ops->read(file->node, file->offset, buf, len, &n);
	if (r == VFS_OK)
		file->offset += n;
	if (done)
		*done = n;
	log_trace("vfs", "read node=%p len=%zu done=%zu status=%d", file->node, len,
			  n, r);
	return r;
}

int vfs_write(vfs_file_t *file, const void *buf, size_t len, size_t *done)
{
	if (done)
		*done = 0;
	if (!file || !buf)
		return VFS_ERR_BADF;
	if ((file->flags & VFS_O_ACCMODE) == VFS_O_RDONLY)
		return VFS_ERR_BADF;
	if (VFS_S_ISDIR(file->node->mode))
		return VFS_ERR_ISDIR;
	if (!file->node->ops || !file->node->ops->write)
		return VFS_ERR_NOSYS;

	uint64_t off =
		(file->flags & VFS_O_APPEND) ? file->node->size : file->offset;
	size_t n = 0;
	int r = file->node->ops->write(file->node, off, buf, len, &n);
	if (r == VFS_OK)
		file->offset = off + n;
	if (done)
		*done = n;
	log_trace("vfs", "write node=%p off=%llu len=%zu done=%zu status=%d",
			  file->node, off, len, n, r);
	return r;
}

int vfs_readdir(vfs_node_t *dir, size_t index, vfs_dirent_t *out)
{
	if (!dir || !out)
		return VFS_ERR_INVAL;
	if (!VFS_S_ISDIR(dir->mode))
		return VFS_ERR_NOTDIR;
	if (!dir->ops || !dir->ops->readdir)
		return VFS_ERR_NOSYS;
	memset(out, 0, sizeof(*out));
	return dir->ops->readdir(dir, index, out);
}

int vfs_seek(vfs_file_t *file, int whence, int64_t off, uint64_t *new_off)
{
	if (!file)
		return VFS_ERR_BADF;

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
		return VFS_ERR_INVAL;
	}

	if (off < 0 && (uint64_t)(-off) > base)
		return VFS_ERR_INVAL;
	file->offset = off < 0 ? base - (uint64_t)(-off) : base + (uint64_t)off;
	if (new_off)
		*new_off = file->offset;
	return VFS_OK;
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
	if (r != VFS_OK)
		return r;
	r = vfs_access(parent, cred, VFS_W_OK | VFS_X_OK);
	if (r == VFS_OK) {
		vfs_mode_t create_mode =
			VFS_S_IFDIR | ((mode & VFS_S_PERM) & ~cred->umask);
		if (parent->ops && parent->ops->mkdir) {
			vfs_node_t *created = NULL;
			r = parent->ops->mkdir(parent, name, name_len, create_mode, cred,
								   &created);
			vfs_node_release(created);
		} else {
			r = VFS_ERR_NOSYS;
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
	if (r != VFS_OK)
		return r;

	r = vfs_access(parent, cred, VFS_W_OK | VFS_X_OK);
	vfs_node_t *victim = NULL;
	if (r == VFS_OK && parent->ops && parent->ops->lookup)
		r = parent->ops->lookup(parent, name, name_len, &victim);
	if (r == VFS_OK && (parent->mode & VFS_S_ISVTX) && cred->uid != 0 &&
		cred->uid != parent->uid && cred->uid != victim->uid)
		r = VFS_ERR_PERM;
	if (r == VFS_OK && dir != VFS_S_ISDIR(victim->mode))
		r = dir ? VFS_ERR_NOTDIR : VFS_ERR_ISDIR;
	if (r == VFS_OK) {
		if (dir && parent->ops && parent->ops->rmdir)
			r = parent->ops->rmdir(parent, name, name_len);
		else if (!dir && parent->ops && parent->ops->unlink)
			r = parent->ops->unlink(parent, name, name_len);
		else
			r = VFS_ERR_NOSYS;
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

int vfs_chmod(const char *path, vfs_mode_t mode, const vfs_cred_t *cred)
{
	cred = _cred_or_root(cred);
	vfs_node_t *node = NULL;
	int r = vfs_resolve(path, cred, &node);
	if (r != VFS_OK)
		return r;
	if (cred->uid != 0 && cred->uid != node->uid) {
		vfs_node_release(node);
		return VFS_ERR_PERM;
	}
	node->mode = (node->mode & VFS_S_IFMT) | (mode & VFS_S_PERM);
	log_trace("vfs", "chmod path=%s mode=0%o", path, node->mode);
	vfs_node_release(node);
	return VFS_OK;
}

int vfs_chown(const char *path, vfs_uid_t uid, vfs_gid_t gid,
			  const vfs_cred_t *cred)
{
	cred = _cred_or_root(cred);
	if (cred->uid != 0)
		return VFS_ERR_PERM;
	vfs_node_t *node = NULL;
	int r = vfs_resolve(path, cred, &node);
	if (r != VFS_OK)
		return r;
	node->uid = uid;
	node->gid = gid;
	node->mode &= ~(VFS_S_ISUID | VFS_S_ISGID);
	vfs_node_release(node);
	return VFS_OK;
}

int vfs_stat(const char *path, const vfs_cred_t *cred, vfs_stat_t *st)
{
	if (!st)
		return VFS_ERR_INVAL;
	vfs_node_t *node = NULL;
	int r = vfs_resolve(path, cred, &node);
	if (r != VFS_OK)
		return r;
	st->mode = node->mode;
	st->uid = node->uid;
	st->gid = node->gid;
	st->size = node->size;
	st->nlink = node->nlink;
	vfs_node_release(node);
	return VFS_OK;
}

int vfs_node_get_page(vfs_node_t *node, uint64_t page_index, int for_write,
					  page_t **out)
{
	if (!node || !out)
		return VFS_ERR_INVAL;
	if (!node->ops || !node->ops->get_page)
		return VFS_ERR_NOSYS;
	return node->ops->get_page(node, page_index, for_write, out);
}

vfs_node_t *vfs_file_node(vfs_file_t *file)
{
	return file ? file->node : NULL;
}
