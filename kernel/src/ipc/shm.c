#include <ipc/ipc.h>
#include <debug/log.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <sync/spinlock.h>

typedef struct ipc_shm_obj {
	ipc_shm_t shm;
	struct ipc_shm_obj *next;
} ipc_shm_obj_t;

static spinlock_t ipc_shm_lock = SPINLOCK_INIT;
static ipc_shm_obj_t *ipc_shm_objects = NULL;

static ipc_shm_obj_t *ipc_shm_find_locked(const char *name)
{
	for (ipc_shm_obj_t *obj = ipc_shm_objects; obj; obj = obj->next) {
		if (strcmp(obj->shm.name, name) == 0)
			return obj;
	}
	return NULL;
}

static ipc_shm_obj_t *ipc_shm_remove_locked(const char *name)
{
	ipc_shm_obj_t **cur = &ipc_shm_objects;
	while (*cur) {
		if (strcmp((*cur)->shm.name, name) == 0) {
			ipc_shm_obj_t *obj = *cur;
			*cur = obj->next;
			obj->next = NULL;
			return obj;
		}
		cur = &(*cur)->next;
	}
	return NULL;
}

static void ipc_copy_shm_name(char *dst, const char *src)
{
	size_t i = 0;
	if (src) {
		for (; i < IPC_SHM_NAME_MAX && src[i]; i++)
			dst[i] = src[i];
	}
	dst[i] = '\0';
}

static int shm_dev_read(void *ctx, uint64_t off, void *buf, size_t len,
						size_t *done)
{
	if (done)
		*done = 0;
	if (!ctx || !buf)
		return IPC_ERR_INVAL;

	ipc_shm_t *shm = ctx;
	if (off >= shm->size || len == 0)
		return IPC_OK;
	size_t n = shm->size - (size_t)off;
	if (n > len)
		n = len;
	memcpy(buf, (const uint8_t *)shm->data + off, n);
	if (done)
		*done = n;
	return IPC_OK;
}

static int shm_dev_write(void *ctx, uint64_t off, const void *buf, size_t len,
						 size_t *done)
{
	if (done)
		*done = 0;
	if (!ctx || !buf)
		return IPC_ERR_INVAL;

	ipc_shm_t *shm = ctx;
	if (off >= shm->size || len == 0)
		return IPC_OK;
	size_t n = shm->size - (size_t)off;
	if (n > len)
		n = len;
	memcpy((uint8_t *)shm->data + off, buf, n);
	if (done)
		*done = n;
	return IPC_OK;
}

int ipc_shm_init(void)
{
	int r = devfs_mkdir("/dev/shm", 0777);
	if (r != 0 && r != -EEXIST)
		return r;
	return IPC_OK;
}

int ipc_shm_create(const char *name, size_t size, ipc_shm_t **out)
{
	if (!name || !*name || size == 0 || !out)
		return IPC_ERR_INVAL;

	ipc_shm_obj_t *obj = kzalloc(sizeof(*obj));
	if (!obj)
		return IPC_ERR_NOMEM;
	void *data = kzalloc(size);
	if (!data) {
		kfree(obj);
		return IPC_ERR_NOMEM;
	}
	ipc_copy_shm_name(obj->shm.name, name);
	obj->shm.size = size;
	obj->shm.data = data;

	spinlock_acquire(&ipc_shm_lock);
	if (ipc_shm_find_locked(obj->shm.name)) {
		ipc_shm_obj_t *existing = ipc_shm_find_locked(obj->shm.name);
		spinlock_release(&ipc_shm_lock);
		kfree(data);
		kfree(obj);
		*out = &existing->shm;
		return IPC_OK;
	}
	obj->next = ipc_shm_objects;
	ipc_shm_objects = obj;
	spinlock_release(&ipc_shm_lock);

	char path[64];
	npf_snprintf(path, sizeof(path), "/dev/shm/%s", obj->shm.name);
	int r = devfs_register_chr(path, 0666, shm_dev_read, shm_dev_write,
							   &obj->shm);
	if (r != IPC_OK)
		log_err("ipc", "failed to publish %s status=%d", path, r);

	*out = &obj->shm;
	return IPC_OK;
}

int ipc_shm_open(const char *name, ipc_shm_t **out)
{
	if (!name || !out)
		return IPC_ERR_INVAL;
	spinlock_acquire(&ipc_shm_lock);
	ipc_shm_obj_t *obj = ipc_shm_find_locked(name);
	spinlock_release(&ipc_shm_lock);
	if (!obj)
		return IPC_ERR_NOENT;
	*out = &obj->shm;
	return IPC_OK;
}

int ipc_shm_unlink(const char *name)
{
	if (!name || !*name)
		return IPC_ERR_INVAL;

	spinlock_acquire(&ipc_shm_lock);
	ipc_shm_obj_t *obj = ipc_shm_remove_locked(name);
	spinlock_release(&ipc_shm_lock);
	if (!obj)
		return IPC_ERR_NOENT;

	char path[64];
	npf_snprintf(path, sizeof(path), "/dev/shm/%s", obj->shm.name);
	int r = devfs_unregister(path);
	kfree(obj->shm.data);
	kfree(obj);
	return r == 0 || r == -ENOENT ? IPC_OK : r;
}
