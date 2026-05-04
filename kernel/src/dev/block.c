#include <dev/block.h>
#include <debug/log.h>
#include <fs/devfs.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <sync/spinlock.h>

static spinlock_t block_lock = SPINLOCK_INIT;
static block_device_t *block_devices;

static void copy_name(char *dst, size_t dst_len, const char *src)
{
	size_t i = 0;
	if (src) {
		for (; i + 1 < dst_len && src[i]; i++)
			dst[i] = src[i];
	}
	dst[i] = 0;
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int block_devfs_read(void *ctx, uint64_t off, void *buf, size_t len,
							size_t *done)
{
	if (done)
		*done = 0;
	if (!ctx || !buf)
		return VFS_ERR_INVAL;
	block_device_t *dev = ctx;
	uint64_t size = dev->block_count * (uint64_t)dev->block_size;
	if (off >= size || len == 0)
		return VFS_OK;
	if (len > size - off)
		len = (size_t)(size - off);
	int r = block_read(dev, off, buf, len);
	if (r == VFS_OK && done)
		*done = len;
	return r;
}

static int block_devfs_write(void *ctx, uint64_t off, const void *buf,
							 size_t len, size_t *done)
{
	if (done)
		*done = 0;
	if (!ctx || !buf)
		return VFS_ERR_INVAL;
	block_device_t *dev = ctx;
	uint64_t size = dev->block_count * (uint64_t)dev->block_size;
	if (off >= size || len == 0)
		return VFS_OK;
	if (len > size - off)
		len = (size_t)(size - off);
	int r = block_write(dev, off, buf, len);
	if (r == VFS_OK && done)
		*done = len;
	return r;
}

static int part_read_blocks(block_device_t *dev, uint64_t lba, uint32_t count,
							void *buf)
{
	if (!dev->parent)
		return VFS_ERR_INVAL;
	return dev->parent->read_blocks(dev->parent, dev->lba_offset + lba, count,
									buf);
}

static int part_write_blocks(block_device_t *dev, uint64_t lba, uint32_t count,
							 const void *buf)
{
	if (!dev->parent || !dev->parent->write_blocks)
		return VFS_ERR_NOSYS;
	return dev->parent->write_blocks(dev->parent, dev->lba_offset + lba, count,
									 buf);
}

static void block_probe_mbr(block_device_t *dev)
{
	if (!dev || dev->parent || dev->block_size < 512 || dev->block_count == 0)
		return;

	uint8_t *mbr = kzalloc(dev->block_size);
	if (!mbr)
		return;
	if (block_read(dev, 0, mbr, 512) != VFS_OK) {
		kfree(mbr);
		return;
	}
	if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
		kfree(mbr);
		return;
	}

	for (size_t i = 0; i < 4; i++) {
		const uint8_t *e = mbr + 446 + i * 16;
		uint8_t type = e[4];
		uint32_t start = rd32(e + 8);
		uint32_t count = rd32(e + 12);
		if (type == 0 || count == 0)
			continue;

		block_device_t part;
		memset(&part, 0, sizeof(part));
		npf_snprintf(part.name, sizeof(part.name), "%sp%zu", dev->name, i + 1);
		part.block_size = dev->block_size;
		part.block_count = count;
		part.lba_offset = start;
		part.parent = dev;
		part.read_blocks = part_read_blocks;
		part.write_blocks = dev->write_blocks ? part_write_blocks : NULL;
		block_register(&part);
	}
	kfree(mbr);
}

int block_system_init(void)
{
	log_info("block", "block layer ok");
	return VFS_OK;
}

int block_register(block_device_t *src)
{
	if (!src || !src->name[0] || !src->read_blocks || src->block_size == 0)
		return VFS_ERR_INVAL;

	block_device_t *dev = kzalloc(sizeof(*dev));
	if (!dev)
		return VFS_ERR_NOMEM;
	memcpy(dev, src, sizeof(*dev));
	dev->next = NULL;
	copy_name(dev->name, sizeof(dev->name), src->name);

	spinlock_acquire(&block_lock);
	for (block_device_t *cur = block_devices; cur; cur = cur->next) {
		if (strcmp(cur->name, dev->name) == 0) {
			spinlock_release(&block_lock);
			kfree(dev);
			return VFS_ERR_EXIST;
		}
	}
	dev->next = block_devices;
	block_devices = dev;
	spinlock_release(&block_lock);

	char path[64];
	npf_snprintf(path, sizeof(path), "/dev/%s", dev->name);
	int r = devfs_register_chr(path, dev->write_blocks ? 0660 : 0440,
							   block_devfs_read, block_devfs_write, dev);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		log_warn("block", "failed to publish %s status=%s(%d)", path,
				 vfs_err_name(r), r);

	log_info("block", "registered %s blocks=%llu size=%u offset=%llu", dev->name,
			 dev->block_count, dev->block_size, dev->lba_offset);

	block_probe_mbr(dev);
	return VFS_OK;
}

int block_read(block_device_t *dev, uint64_t off, void *buf, size_t len)
{
	if (!dev || !buf)
		return VFS_ERR_INVAL;
	if (len == 0)
		return VFS_OK;
	uint8_t *bounce = NULL;
	uint32_t bs = dev->block_size;
	if (bs == 0)
		return VFS_ERR_INVAL;

	if ((off % bs) == 0 && (len % bs) == 0)
		return dev->read_blocks(dev, off / bs, (uint32_t)(len / bs), buf);

	bounce = kzalloc(bs);
	if (!bounce)
		return VFS_ERR_NOMEM;
	size_t done = 0;
	while (done < len) {
		uint64_t pos = off + done;
		uint64_t lba = pos / bs;
		size_t boff = (size_t)(pos % bs);
		size_t chunk = bs - boff;
		if (chunk > len - done)
			chunk = len - done;
		int r = dev->read_blocks(dev, lba, 1, bounce);
		if (r != VFS_OK) {
			kfree(bounce);
			return r;
		}
		memcpy((uint8_t *)buf + done, bounce + boff, chunk);
		done += chunk;
	}
	kfree(bounce);
	return VFS_OK;
}

int block_write(block_device_t *dev, uint64_t off, const void *buf, size_t len)
{
	if (!dev || !buf)
		return VFS_ERR_INVAL;
	if (!dev->write_blocks)
		return VFS_ERR_NOSYS;
	if (len == 0)
		return VFS_OK;
	uint32_t bs = dev->block_size;
	if ((off % bs) == 0 && (len % bs) == 0)
		return dev->write_blocks(dev, off / bs, (uint32_t)(len / bs), buf);

	uint8_t *bounce = kzalloc(bs);
	if (!bounce)
		return VFS_ERR_NOMEM;
	size_t done = 0;
	while (done < len) {
		uint64_t pos = off + done;
		uint64_t lba = pos / bs;
		size_t boff = (size_t)(pos % bs);
		size_t chunk = bs - boff;
		if (chunk > len - done)
			chunk = len - done;
		int r = dev->read_blocks(dev, lba, 1, bounce);
		if (r == VFS_OK) {
			memcpy(bounce + boff, (const uint8_t *)buf + done, chunk);
			r = dev->write_blocks(dev, lba, 1, bounce);
		}
		if (r != VFS_OK) {
			kfree(bounce);
			return r;
		}
		done += chunk;
	}
	kfree(bounce);
	return VFS_OK;
}

block_device_t *block_first(void)
{
	return block_devices;
}

block_device_t *block_find(const char *name)
{
	for (block_device_t *dev = block_devices; dev; dev = dev->next) {
		if (strcmp(dev->name, name) == 0)
			return dev;
	}
	return NULL;
}
