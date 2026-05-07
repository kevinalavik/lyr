#include <dev/device.h>
#include <debug/log.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <sync/spinlock.h>

static spinlock_t device_lock = SPINLOCK_INIT;
static device_t *devices;
static device_handler_t *handlers;

extern int npf_snprintf_(char *buffer, size_t bufsz, const char *format, ...);

static void copy_name(char *dst, size_t len, const char *src)
{
	size_t i = 0;
	if (src) {
		for (; i + 1 < len && src[i]; i++)
			dst[i] = src[i];
	}
	dst[i] = 0;
}

static int device_info_read(void *ctx, uint64_t off, void *buf, size_t len,
							size_t *done)
{
	if (done)
		*done = 0;
	if (!ctx || !buf)
		return VFS_ERR_INVAL;

	const device_t *dev = ctx;
	char tmp[512];
	int n = 0;

	if (dev->bus_type == DEVICE_BUS_PCI) {
		const device_pci_info_t *p = &dev->pci;
		n = npf_snprintf(
			tmp, sizeof(tmp),
			"name=%s\nbus=pci\naddress=0000:%02x:%02x.%u\nvendor=%04x\ndevice=%04x\nclass=%02x\nsubclass=%02x\nprog_if=%02x\nrevision=%02x\nhandler=%s\n",
			dev->name, p->bus, p->slot, p->function, p->vendor_id, p->device_id,
			p->class_code, p->subclass, p->prog_if, p->revision,
			dev->handler ? dev->handler->name : "none");
	} else if (dev->bus_type == DEVICE_BUS_PLATFORM) {
		n = npf_snprintf(tmp, sizeof(tmp), "name=%s\nbus=platform\nhandler=%s\n",
						 dev->name, dev->handler ? dev->handler->name : "none");
	} else {
		n = npf_snprintf(tmp, sizeof(tmp), "name=%s\nbus=unknown\nhandler=%s\n",
						 dev->name, dev->handler ? dev->handler->name : "none");
	}

	if (n < 0)
		return VFS_ERR_INVAL;
	size_t total = (size_t)n;
	if (total >= sizeof(tmp))
		total = sizeof(tmp) - 1;
	if (off >= total || len == 0)
		return VFS_OK;

	size_t copy = total - (size_t)off;
	if (copy > len)
		copy = len;
	memcpy(buf, tmp + off, copy);
	if (done)
		*done = copy;
	return VFS_OK;
}

static int publish_device(device_t *dev)
{
	char path[96];
	npf_snprintf(path, sizeof(path), "/dev/devices/%s", dev->name);
	int r = devfs_mkdir(path, 0755);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;

	char info[128];
	npf_snprintf(info, sizeof(info), "%s/info", path);
	r = devfs_register_chr(info, 0444, device_info_read, NULL, dev);
	return (r == VFS_ERR_EXIST) ? VFS_OK : r;
}

static int try_bind(device_t *dev, device_handler_t *handler)
{
	if (dev->handler || dev->bus_type != handler->bus_type)
		return VFS_OK;
	if (handler->match && !handler->match(dev, handler->ctx))
		return VFS_OK;

	int r = handler->probe ? handler->probe(dev, handler->ctx) : VFS_OK;
	if (r == VFS_OK) {
		dev->handler = handler;
		log_debug("device", "%s bound to %s", dev->name, handler->name);
	}
	return r;
}

int device_system_init(void)
{
	int r = devfs_mkdir("/dev/devices", 0755);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;
	log_debug("device", "device registry ok");
	return VFS_OK;
}

int device_register(device_t *src)
{
	if (!src || !src->name[0])
		return VFS_ERR_INVAL;

	device_t *dev = kzalloc(sizeof(*dev));
	if (!dev)
		return VFS_ERR_NOMEM;
	memcpy(dev, src, sizeof(*dev));
	dev->next = NULL;
	dev->handler = NULL;
	copy_name(dev->name, sizeof(dev->name), src->name);

	spinlock_acquire(&device_lock);
	for (device_t *cur = devices; cur; cur = cur->next) {
		if (strcmp(cur->name, dev->name) == 0) {
			spinlock_release(&device_lock);
			kfree(dev);
			return VFS_ERR_EXIST;
		}
	}
	dev->next = devices;
	devices = dev;
	spinlock_release(&device_lock);

	publish_device(dev);

	for (device_handler_t *h = handlers; h; h = h->next)
		try_bind(dev, h);
	return VFS_OK;
}

int device_handler_register(device_handler_t *src)
{
	if (!src || !src->name[0])
		return VFS_ERR_INVAL;

	device_handler_t *handler = kzalloc(sizeof(*handler));
	if (!handler)
		return VFS_ERR_NOMEM;
	memcpy(handler, src, sizeof(*handler));
	handler->next = NULL;
	copy_name(handler->name, sizeof(handler->name), src->name);

	spinlock_acquire(&device_lock);
	handler->next = handlers;
	handlers = handler;
	spinlock_release(&device_lock);

	log_info("device", "handler %s registered", handler->name);

	for (device_t *dev = devices; dev; dev = dev->next)
		try_bind(dev, handler);
	return VFS_OK;
}

device_t *device_first(void)
{
	return devices;
}
