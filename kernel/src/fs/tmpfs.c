#include <fs/tmpfs.h>
#include <debug/log.h>
#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/pfndb.h>
#include <lib/string.h>

typedef struct tmpfs_node tmpfs_node_t;

struct tmpfs_node {
	vfs_node_t vnode;
	tmpfs_node_t *parent;
	tmpfs_node_t *children;
	tmpfs_node_t *next;
	char name[VFS_NAME_MAX + 1];
	page_t **pages;
	size_t page_count;
	size_t page_cap;
};

static int tmpfs_lookup(vfs_node_t *dir, const char *name, size_t len,
						vfs_node_t **out);
static int tmpfs_create(vfs_node_t *dir, const char *name, size_t len,
						vfs_mode_t mode, const vfs_cred_t *cred,
						vfs_node_t **out);
static int tmpfs_mkdir(vfs_node_t *dir, const char *name, size_t len,
					   vfs_mode_t mode, const vfs_cred_t *cred,
					   vfs_node_t **out);
static int tmpfs_unlink(vfs_node_t *dir, const char *name, size_t len);
static int tmpfs_rmdir(vfs_node_t *dir, const char *name, size_t len);
static int tmpfs_read(vfs_node_t *node, uint64_t off, void *buf, size_t len,
					  size_t *done);
static int tmpfs_write(vfs_node_t *node, uint64_t off, const void *buf,
					   size_t len, size_t *done);
static int tmpfs_readdir(vfs_node_t *dir, size_t index, vfs_dirent_t *out);
static int tmpfs_truncate(vfs_node_t *node, uint64_t size);
static int tmpfs_get_page(vfs_node_t *node, uint64_t page_index, int for_write,
						  page_t **out);
static void tmpfs_release(vfs_node_t *node);

static const vfs_ops_t tmpfs_ops = {
	.lookup = tmpfs_lookup,
	.create = tmpfs_create,
	.mkdir = tmpfs_mkdir,
	.unlink = tmpfs_unlink,
	.rmdir = tmpfs_rmdir,
	.read = tmpfs_read,
	.write = tmpfs_write,
	.readdir = tmpfs_readdir,
	.truncate = tmpfs_truncate,
	.get_page = tmpfs_get_page,
	.release = tmpfs_release,
};

static tmpfs_node_t *_to_tmpfs(vfs_node_t *node)
{
	return (tmpfs_node_t *)node;
}

static int _name_eq(tmpfs_node_t *node, const char *name, size_t len)
{
	return strlen(node->name) == len && memcmp(node->name, name, len) == 0;
}

static tmpfs_node_t *_find_child(tmpfs_node_t *dir, const char *name,
								 size_t len)
{
	for (tmpfs_node_t *child = dir->children; child; child = child->next) {
		if (_name_eq(child, name, len))
			return child;
	}
	return NULL;
}

static tmpfs_node_t *_alloc_node(const char *name, size_t len, vfs_mode_t mode,
								 vfs_uid_t uid, vfs_gid_t gid)
{
	if (len > VFS_NAME_MAX)
		return NULL;

	tmpfs_node_t *node = kzalloc(sizeof(*node));
	if (!node)
		return NULL;
	vfs_node_init(&node->vnode, &tmpfs_ops, mode, uid, gid);
	memcpy(node->name, name, len);
	node->name[len] = '\0';
	node->vnode.private_data = node;
	return node;
}

static void _insert_child(tmpfs_node_t *dir, tmpfs_node_t *child)
{
	child->parent = dir;
	child->next = dir->children;
	dir->children = child;
	if (VFS_S_ISDIR(child->vnode.mode))
		dir->vnode.nlink++;
}

static void _unlink_child(tmpfs_node_t *dir, tmpfs_node_t *child)
{
	tmpfs_node_t **cur = &dir->children;
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

static int _ensure_pages(tmpfs_node_t *node, size_t count)
{
	if (count <= node->page_cap)
		return VFS_OK;

	size_t new_cap = node->page_cap ? node->page_cap : 4;
	while (new_cap < count)
		new_cap *= 2;

	page_t **pages = krealloc(node->pages, new_cap * sizeof(page_t *));
	if (!pages)
		return VFS_ERR_NOMEM;
	memset(pages + node->page_cap, 0,
		   (new_cap - node->page_cap) * sizeof(page_t *));
	node->pages = pages;
	node->page_cap = new_cap;
	return VFS_OK;
}

static int _ensure_page(tmpfs_node_t *node, size_t index, page_t **out)
{
	int r = _ensure_pages(node, index + 1);
	if (r != VFS_OK)
		return r;
	if (!node->pages[index]) {
		page_t *page = palloc_page();
		if (!page)
			return VFS_ERR_NOMEM;
		memset(PHYS_TO_VIRT(pfndb_page_to_phys(page)), 0, PAGE_SIZE);
		node->pages[index] = page;
	}
	if (index + 1 > node->page_count)
		node->page_count = index + 1;
	*out = node->pages[index];
	return VFS_OK;
}

static void _free_pages_from(tmpfs_node_t *node, size_t first)
{
	if (!node->pages)
		return;
	for (size_t i = first; i < node->page_count; i++) {
		if (node->pages[i]) {
			page_unref(node->pages[i]);
			node->pages[i] = NULL;
		}
	}
	if (first < node->page_count)
		node->page_count = first;
}

vfs_node_t *tmpfs_create_root(vfs_mode_t mode, vfs_uid_t uid, vfs_gid_t gid)
{
	tmpfs_node_t *root =
		_alloc_node("", 0, VFS_S_IFDIR | (mode & VFS_S_PERM), uid, gid);
	if (!root)
		return NULL;
	root->parent = root;
	log_debug("tmpfs", "created root mode=0%o uid=%u gid=%u", root->vnode.mode,
			  uid, gid);
	return &root->vnode;
}

static int tmpfs_lookup(vfs_node_t *dir_node, const char *name, size_t len,
						vfs_node_t **out)
{
	if (!VFS_S_ISDIR(dir_node->mode))
		return VFS_ERR_NOTDIR;
	if (len == 1 && name[0] == '.') {
		vfs_node_ref(dir_node);
		*out = dir_node;
		return VFS_OK;
	}

	tmpfs_node_t *dir = _to_tmpfs(dir_node);
	if (len == 2 && name[0] == '.' && name[1] == '.') {
		vfs_node_t *parent = &dir->parent->vnode;
		vfs_node_ref(parent);
		*out = parent;
		return VFS_OK;
	}

	tmpfs_node_t *child = _find_child(dir, name, len);
	if (!child)
		return VFS_ERR_NOENT;
	vfs_node_ref(&child->vnode);
	*out = &child->vnode;
	log_trace("tmpfs", "lookup %.*s -> node=%p", (int)len, name, &child->vnode);
	return VFS_OK;
}

static int _create_child(vfs_node_t *dir_node, const char *name, size_t len,
						 vfs_mode_t mode, const vfs_cred_t *cred,
						 vfs_node_t **out)
{
	if (!VFS_S_ISDIR(dir_node->mode))
		return VFS_ERR_NOTDIR;
	if (len == 0 || len > VFS_NAME_MAX)
		return len == 0 ? VFS_ERR_INVAL : VFS_ERR_NAMETOOLONG;

	tmpfs_node_t *dir = _to_tmpfs(dir_node);
	if (_find_child(dir, name, len))
		return VFS_ERR_EXIST;

	tmpfs_node_t *child = _alloc_node(name, len, mode, cred->uid, cred->gid);
	if (!child)
		return VFS_ERR_NOMEM;
	_insert_child(dir, child);
	vfs_node_ref(&child->vnode);
	*out = &child->vnode;
	log_debug("tmpfs", "create %.*s node=%p mode=0%o", (int)len, name,
			  &child->vnode, child->vnode.mode);
	return VFS_OK;
}

static int tmpfs_create(vfs_node_t *dir, const char *name, size_t len,
						vfs_mode_t mode, const vfs_cred_t *cred,
						vfs_node_t **out)
{
	return _create_child(dir, name, len, VFS_S_IFREG | (mode & VFS_S_PERM),
						 cred, out);
}

static int tmpfs_mkdir(vfs_node_t *dir, const char *name, size_t len,
					   vfs_mode_t mode, const vfs_cred_t *cred,
					   vfs_node_t **out)
{
	return _create_child(dir, name, len, VFS_S_IFDIR | (mode & VFS_S_PERM),
						 cred, out);
}

static int tmpfs_unlink(vfs_node_t *dir_node, const char *name, size_t len)
{
	tmpfs_node_t *dir = _to_tmpfs(dir_node);
	tmpfs_node_t *child = _find_child(dir, name, len);
	if (!child)
		return VFS_ERR_NOENT;
	if (VFS_S_ISDIR(child->vnode.mode))
		return VFS_ERR_ISDIR;
	_unlink_child(dir, child);
	child->vnode.nlink = 0;
	vfs_node_release(&child->vnode);
	return VFS_OK;
}

static int tmpfs_rmdir(vfs_node_t *dir_node, const char *name, size_t len)
{
	tmpfs_node_t *dir = _to_tmpfs(dir_node);
	tmpfs_node_t *child = _find_child(dir, name, len);
	if (!child)
		return VFS_ERR_NOENT;
	if (!VFS_S_ISDIR(child->vnode.mode))
		return VFS_ERR_NOTDIR;
	if (child->children)
		return VFS_ERR_NOTEMPTY;
	_unlink_child(dir, child);
	child->vnode.nlink = 0;
	vfs_node_release(&child->vnode);
	return VFS_OK;
}

static int tmpfs_read(vfs_node_t *node, uint64_t off, void *buf, size_t len,
					  size_t *done)
{
	if (!VFS_S_ISREG(node->mode))
		return VFS_ERR_ISDIR;
	tmpfs_node_t *tn = _to_tmpfs(node);
	if (off >= node->size || len == 0) {
		if (done)
			*done = 0;
		return VFS_OK;
	}

	size_t todo = len;
	if (todo > node->size - off)
		todo = (size_t)(node->size - off);
	size_t copied = 0;
	while (copied < todo) {
		uint64_t pos = off + copied;
		size_t page_index = (size_t)(pos / PAGE_SIZE);
		size_t page_off = (size_t)(pos & (PAGE_SIZE - 1));
		size_t chunk = PAGE_SIZE - page_off;
		if (chunk > todo - copied)
			chunk = todo - copied;

		if (page_index < tn->page_count && tn->pages[page_index]) {
			void *src = PHYS_TO_VIRT(pfndb_page_to_phys(tn->pages[page_index]));
			memcpy((uint8_t *)buf + copied, (uint8_t *)src + page_off, chunk);
		} else {
			memset((uint8_t *)buf + copied, 0, chunk);
		}
		copied += chunk;
	}
	if (done)
		*done = copied;
	log_trace("tmpfs", "read node=%p off=%llu len=%zu done=%zu", node, off, len,
			  copied);
	return VFS_OK;
}

static int tmpfs_write(vfs_node_t *node, uint64_t off, const void *buf,
					   size_t len, size_t *done)
{
	if (!VFS_S_ISREG(node->mode))
		return VFS_ERR_ISDIR;
	tmpfs_node_t *tn = _to_tmpfs(node);
	size_t copied = 0;
	while (copied < len) {
		uint64_t pos = off + copied;
		size_t page_index = (size_t)(pos / PAGE_SIZE);
		size_t page_off = (size_t)(pos & (PAGE_SIZE - 1));
		size_t chunk = PAGE_SIZE - page_off;
		if (chunk > len - copied)
			chunk = len - copied;

		page_t *page = NULL;
		int r = _ensure_page(tn, page_index, &page);
		if (r != VFS_OK) {
			if (done)
				*done = copied;
			return copied ? VFS_OK : r;
		}
		void *dst = PHYS_TO_VIRT(pfndb_page_to_phys(page));
		memcpy((uint8_t *)dst + page_off, (const uint8_t *)buf + copied, chunk);
		copied += chunk;
	}

	if (off + copied > node->size)
		node->size = off + copied;
	if (done)
		*done = copied;
	log_trace("tmpfs", "write node=%p off=%llu len=%zu done=%zu size=%llu",
			  node, off, len, copied, node->size);
	return VFS_OK;
}

static int tmpfs_readdir(vfs_node_t *dir_node, size_t index, vfs_dirent_t *out)
{
	if (!VFS_S_ISDIR(dir_node->mode))
		return VFS_ERR_NOTDIR;
	if (!out)
		return VFS_ERR_INVAL;

	tmpfs_node_t *dir = _to_tmpfs(dir_node);
	size_t i = 0;
	for (tmpfs_node_t *child = dir->children; child; child = child->next) {
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
		return VFS_OK;
	}
	return VFS_ERR_NOENT;
}

static int tmpfs_truncate(vfs_node_t *node, uint64_t size)
{
	if (!VFS_S_ISREG(node->mode))
		return VFS_ERR_ISDIR;
	tmpfs_node_t *tn = _to_tmpfs(node);
	size_t keep_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	if (keep_pages < tn->page_count)
		_free_pages_from(tn, keep_pages);
	if (size > node->size && keep_pages > 0) {
		page_t *page = NULL;
		int r = _ensure_page(tn, keep_pages - 1, &page);
		if (r != VFS_OK)
			return r;
	}
	node->size = size;
	if ((size & (PAGE_SIZE - 1)) && keep_pages > 0 &&
		keep_pages - 1 < tn->page_count && tn->pages[keep_pages - 1]) {
		size_t tail = (size_t)(size & (PAGE_SIZE - 1));
		void *page =
			PHYS_TO_VIRT(pfndb_page_to_phys(tn->pages[keep_pages - 1]));
		memset((uint8_t *)page + tail, 0, PAGE_SIZE - tail);
	}
	log_debug("tmpfs", "truncate node=%p size=%llu", node, size);
	return VFS_OK;
}

static int tmpfs_get_page(vfs_node_t *node, uint64_t page_index, int for_write,
						  page_t **out)
{
	if (!VFS_S_ISREG(node->mode))
		return VFS_ERR_ISDIR;
	tmpfs_node_t *tn = _to_tmpfs(node);
	if (page_index > (uint64_t)(SIZE_MAX - 1))
		return VFS_ERR_INVAL;
	return _ensure_page(tn, (size_t)page_index, out);
}

static void tmpfs_release(vfs_node_t *node)
{
	tmpfs_node_t *tn = _to_tmpfs(node);
	while (tn->children) {
		tmpfs_node_t *child = tn->children;
		_unlink_child(tn, child);
		child->vnode.nlink = 0;
		vfs_node_release(&child->vnode);
	}
	_free_pages_from(tn, 0);
	kfree(tn->pages);
	kfree(tn);
}
