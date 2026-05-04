#include <ipc/ipc.h>
#include <debug/log.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <sync/spinlock.h>

typedef struct ipc_endpoint {
	char name[IPC_NAME_MAX + 1];
	int32_t owner;
	ipc_handler_t handler;
	void *ctx;
	struct ipc_endpoint *next;
} ipc_endpoint_t;

static spinlock_t ipc_unix_lock = SPINLOCK_INIT;
static ipc_endpoint_t *ipc_endpoints = NULL;

static void ipc_copy_name(char *dst, const char *src)
{
	size_t i = 0;
	if (src) {
		for (; i < IPC_NAME_MAX && src[i]; i++)
			dst[i] = src[i];
	}
	dst[i] = '\0';
}

static ipc_endpoint_t *ipc_find_locked(const char *name)
{
	for (ipc_endpoint_t *ep = ipc_endpoints; ep; ep = ep->next) {
		if (strcmp(ep->name, name) == 0)
			return ep;
	}
	return NULL;
}

static ipc_endpoint_t *ipc_remove_locked(const char *name)
{
	ipc_endpoint_t **cur = &ipc_endpoints;
	while (*cur) {
		if (strcmp((*cur)->name, name) == 0) {
			ipc_endpoint_t *ep = *cur;
			*cur = ep->next;
			ep->next = NULL;
			return ep;
		}
		cur = &(*cur)->next;
	}
	return NULL;
}

static int endpoint_dev_read(void *ctx, uint64_t off, void *buf, size_t len,
							 size_t *done)
{
	if (done)
		*done = 0;
	if (!ctx || !buf)
		return IPC_ERR_INVAL;

	ipc_endpoint_t *ep = ctx;
	char tmp[128];
	int n = npf_snprintf(tmp, sizeof(tmp), "name=%s\nowner=%d\n",
						 ep->name, ep->owner);
	if (n < 0)
		return IPC_ERR_INVAL;
	size_t total = (size_t)n;
	if (total >= sizeof(tmp))
		total = sizeof(tmp) - 1;
	if (off >= total)
		return IPC_OK;
	size_t copy = total - (size_t)off;
	if (copy > len)
		copy = len;
	memcpy(buf, tmp + off, copy);
	if (done)
		*done = copy;
	return IPC_OK;
}

int ipc_unix_init(void)
{
	int r = devfs_mkdir("/dev/ipc", 0755);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;
	return IPC_OK;
}

int ipc_endpoint_register(const char *name, int32_t owner,
						  ipc_handler_t handler, void *ctx)
{
	if (!name || !*name || !handler)
		return IPC_ERR_INVAL;

	ipc_endpoint_t *ep = kzalloc(sizeof(*ep));
	if (!ep)
		return IPC_ERR_NOMEM;
	ipc_copy_name(ep->name, name);
	ep->owner = owner;
	ep->handler = handler;
	ep->ctx = ctx;

	spinlock_acquire(&ipc_unix_lock);
	if (ipc_find_locked(ep->name)) {
		spinlock_release(&ipc_unix_lock);
		kfree(ep);
		return IPC_ERR_EXIST;
	}
	ep->next = ipc_endpoints;
	ipc_endpoints = ep;
	spinlock_release(&ipc_unix_lock);

	char path[64];
	npf_snprintf(path, sizeof(path), "/dev/ipc/%s", ep->name);
	int r = devfs_register_chr(path, 0444, endpoint_dev_read, NULL, ep);
	if (r != IPC_OK)
		log_err("ipc", "failed to publish %s status=%d", path, r);

	log_debug("ipc", "endpoint %s owned by pid=%d", ep->name, ep->owner);
	return IPC_OK;
}

int ipc_endpoint_unregister(const char *name)
{
	if (!name || !*name)
		return IPC_ERR_INVAL;

	spinlock_acquire(&ipc_unix_lock);
	ipc_endpoint_t *ep = ipc_remove_locked(name);
	spinlock_release(&ipc_unix_lock);
	if (!ep)
		return IPC_ERR_NOENT;

	char path[64];
	npf_snprintf(path, sizeof(path), "/dev/ipc/%s", ep->name);
	int r = devfs_unregister(path);
	kfree(ep);
	return r == VFS_OK || r == VFS_ERR_NOENT ? IPC_OK : r;
}

int ipc_call(const char *name, const ipc_msg_t *msg)
{
	if (!name || !msg)
		return IPC_ERR_INVAL;

	spinlock_acquire(&ipc_unix_lock);
	ipc_endpoint_t *ep = ipc_find_locked(name);
	ipc_handler_t handler = ep ? ep->handler : NULL;
	void *ctx = ep ? ep->ctx : NULL;
	spinlock_release(&ipc_unix_lock);
	if (!handler)
		return IPC_ERR_NOENT;
	return handler(msg, ctx);
}

int ipc_notify(const char *name, uint32_t type, const void *in, size_t in_len)
{
	ipc_msg_t msg = {
		.kind = IPC_MSG_NOTIFY,
		.type = type,
		.in = in,
		.in_len = in_len,
	};
	return ipc_call(name, &msg);
}

int ipc_snapshot(char *buf, size_t len)
{
	if (!buf || len == 0)
		return IPC_ERR_INVAL;

	size_t off = 0;
	spinlock_acquire(&ipc_unix_lock);
	for (ipc_endpoint_t *ep = ipc_endpoints; ep && off < len; ep = ep->next) {
		int n = npf_snprintf(buf + off, len - off, "%s pid=%d\n", ep->name,
							 ep->owner);
		if (n < 0)
			break;
		size_t wrote = (size_t)n;
		if (wrote >= len - off) {
			off = len - 1;
			break;
		}
		off += wrote;
	}
	spinlock_release(&ipc_unix_lock);
	buf[off < len ? off : len - 1] = '\0';
	return IPC_OK;
}
