#include <cpu/instr.h>
#include <drv/driver.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <ipc/ipc.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>
#include "pci.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_IDS_MAX (2 * 1024 * 1024)

static pci_device_list_t pci_devices;
static char *pci_ids;
static size_t pci_ids_len;

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int parse_hex4(const char *s, uint16_t *out)
{
	int a = hexval(s[0]), b = hexval(s[1]), c = hexval(s[2]), d = hexval(s[3]);
	if (a < 0 || b < 0 || c < 0 || d < 0)
		return 0;
	*out = (uint16_t)((a << 12) | (b << 8) | (c << 4) | d);
	return 1;
}

static void copy_trimmed(char *dst, size_t dst_len, const char *s, size_t len)
{
	if (!dst_len)
		return;

	while (len && (*s == ' ' || *s == '\t')) {
		s++;
		len--;
	}

	while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
				   s[len - 1] == '\r' || s[len - 1] == '\n')) {
		len--;
	}

	size_t n = len < dst_len - 1 ? len : dst_len - 1;
	memcpy(dst, s, n);
	dst[n] = 0;
}

static void pci_ids_lookup(uint16_t vendor, uint16_t device, char *vendor_name,
						   size_t vendor_name_len, char *device_name,
						   size_t device_name_len)
{
	if (vendor_name_len)
		vendor_name[0] = 0;
	if (device_name_len)
		device_name[0] = 0;

	if (!pci_ids || !pci_ids_len)
		return;

	int in_vendor = 0;

	for (size_t pos = 0; pos < pci_ids_len;) {
		size_t line_start = pos;

		while (pos < pci_ids_len && pci_ids[pos] != '\n')
			pos++;

		size_t line_len = pos - line_start;

		if (pos < pci_ids_len && pci_ids[pos] == '\n')
			pos++;

		const char *line = &pci_ids[line_start];

		if (!line_len || line[0] == '#')
			continue;

		if (line[0] != '\t') {
			uint16_t v;
			if (line_len >= 6 && parse_hex4(line, &v) &&
				(line[4] == ' ' || line[4] == '\t')) {
				in_vendor = (v == vendor);
				if (in_vendor) {
					copy_trimmed(vendor_name, vendor_name_len, line + 5,
								 line_len - 5);
				}
			} else {
				in_vendor = 0;
			}
			continue;
		}

		if (!in_vendor)
			continue;

		if (line_len >= 7 && line[0] == '\t' && line[1] != '\t') {
			uint16_t d;
			if (parse_hex4(line + 1, &d) &&
				(line[5] == ' ' || line[5] == '\t') && d == device) {
				copy_trimmed(device_name, device_name_len, line + 6,
							 line_len - 6);
				return;
			}
		}
	}
}

static void pci_load_ids(driver_t *driver)
{
	vfs_file_t *file = NULL;
	int r = vfs_open("/sys/pci.ids", VFS_O_RDONLY, 0, NULL, &file);
	if (r != VFS_OK) {
		driver_log(driver, "warn", "could not open /sys/pci.ids");
		return;
	}

	pci_ids = kzalloc(PCI_IDS_MAX);
	if (!pci_ids) {
		vfs_close(file);
		driver_log(driver, "warn", "could not allocate pci.ids buffer");
		return;
	}

	pci_ids_len = 0;

	while (pci_ids_len < PCI_IDS_MAX) {
		size_t done = 0;
		r = vfs_read(file, pci_ids + pci_ids_len, PCI_IDS_MAX - pci_ids_len,
					 &done);
		if (r != VFS_OK || done == 0)
			break;
		pci_ids_len += done;
	}

	vfs_close(file);

	if (!pci_ids_len) {
		kfree(pci_ids);
		pci_ids = NULL;
		driver_log(driver, "warn", "/sys/pci.ids empty or unreadable");
	}
}

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t function,
						   uint8_t offset)
{
	uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) |
					((uint32_t)slot << 11) | ((uint32_t)function << 8) |
					(offset & 0xFC);
	outl(PCI_CONFIG_ADDR, addr);
	return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t function,
						   uint8_t offset)
{
	uint32_t v = pci_read32(bus, slot, function, offset);
	return (uint16_t)(v >> ((offset & 2) * 8));
}

static uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t function,
						 uint8_t offset)
{
	uint32_t v = pci_read32(bus, slot, function, offset);
	return (uint8_t)(v >> ((offset & 3) * 8));
}

static void pci_scan_function(uint8_t bus, uint8_t slot, uint8_t function)
{
	uint16_t vendor = pci_read16(bus, slot, function, 0x00);
	if (vendor == 0xFFFF || pci_devices.count >= PCI_MAX_SNAPSHOT_DEVICES)
		return;

	pci_device_info_t *dev = &pci_devices.devices[pci_devices.count++];
	dev->bus = bus;
	dev->slot = slot;
	dev->function = function;
	dev->vendor_id = vendor;
	dev->device_id = pci_read16(bus, slot, function, 0x02);
	dev->revision = pci_read8(bus, slot, function, 0x08);
	dev->prog_if = pci_read8(bus, slot, function, 0x09);
	dev->subclass = pci_read8(bus, slot, function, 0x0A);
	dev->class_code = pci_read8(bus, slot, function, 0x0B);
}

static void pci_scan(void)
{
	memset(&pci_devices, 0, sizeof(pci_devices));

	for (uint16_t bus = 0; bus < 256; bus++) {
		for (uint8_t slot = 0; slot < 32; slot++) {
			uint16_t vendor = pci_read16((uint8_t)bus, slot, 0, 0x00);
			if (vendor == 0xFFFF)
				continue;

			pci_scan_function((uint8_t)bus, slot, 0);

			uint8_t header = pci_read8((uint8_t)bus, slot, 0, 0x0E);
			if (!(header & 0x80))
				continue;

			for (uint8_t fn = 1; fn < 8; fn++)
				pci_scan_function((uint8_t)bus, slot, fn);
		}
	}
}

static int pci_ipc_handler(const ipc_msg_t *msg, void *ctx)
{
	(void)ctx;

	if (!msg || msg->type != PCI_IPC_LIST_DEVICES || !msg->out)
		return IPC_ERR_INVAL;

	size_t n = sizeof(pci_devices);
	if (n > msg->out_len)
		n = msg->out_len;

	memcpy(msg->out, &pci_devices, n);

	if (msg->actual)
		*msg->actual = n;

	return IPC_OK;
}

static int copy_text_slice(const char *tmp, size_t total, uint64_t off,
						   void *buf, size_t len, size_t *done)
{
	if (done)
		*done = 0;

	if (!buf)
		return VFS_ERR_INVAL;

	if (off >= total || len == 0)
		return 0;

	size_t copy = total - (size_t)off;
	if (copy > len)
		copy = len;

	memcpy(buf, tmp + off, copy);

	if (done)
		*done = copy;

	return 0;
}

static int pci_dev_read(void *ctx, uint64_t off, void *buf, size_t len,
						size_t *done)
{
	(void)ctx;

	if (done)
		*done = 0;

	if (!buf)
		return VFS_ERR_INVAL;

	char tmp[4096];
	size_t pos = 0;

	int n = npf_snprintf(tmp, sizeof(tmp), "count=%zu\n", pci_devices.count);
	if (n < 0)
		return VFS_ERR_INVAL;

	pos = (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1;

	for (size_t i = 0; i < pci_devices.count && i < PCI_MAX_SNAPSHOT_DEVICES &&
					   pos < sizeof(tmp);
		 i++) {
		const pci_device_info_t *dev = &pci_devices.devices[i];

		char vendor_name[96];
		char device_name[128];

		pci_ids_lookup(dev->vendor_id, dev->device_id, vendor_name,
					   sizeof(vendor_name), device_name, sizeof(device_name));

		n = npf_snprintf(
			tmp + pos, sizeof(tmp) - pos,
			"%02x:%02x.%u vendor=%04x device=%04x class=%02x:%02x:%02x rev=%02x vendor_name=\"%s\" device_name=\"%s\"\n",
			dev->bus, dev->slot, dev->function, dev->vendor_id, dev->device_id,
			dev->class_code, dev->subclass, dev->prog_if, dev->revision,
			vendor_name[0] ? vendor_name : "?",
			device_name[0] ? device_name : "?");

		if (n < 0)
			break;

		if ((size_t)n >= sizeof(tmp) - pos) {
			pos = sizeof(tmp) - 1;
			break;
		}

		pos += (size_t)n;
	}

	return copy_text_slice(tmp, pos, off, buf, len, done);
}

static int pci_device_info_read(void *ctx, uint64_t off, void *buf, size_t len,
								size_t *done)
{
	if (!ctx)
		return VFS_ERR_INVAL;

	const pci_device_info_t *dev = ctx;

	char vendor_name[96];
	char device_name[128];

	pci_ids_lookup(dev->vendor_id, dev->device_id, vendor_name,
				   sizeof(vendor_name), device_name, sizeof(device_name));

	char tmp[768];
	int n = npf_snprintf(
		tmp, sizeof(tmp),
		"address=0000:%02x:%02x.%u\nvendor=%04x\ndevice=%04x\nvendor_name=%s\ndevice_name=%s\nclass=%02x\nsubclass=%02x\nprog_if=%02x\nrevision=%02x\n",
		dev->bus, dev->slot, dev->function, dev->vendor_id, dev->device_id,
		vendor_name[0] ? vendor_name : "?", device_name[0] ? device_name : "?",
		dev->class_code, dev->subclass, dev->prog_if, dev->revision);

	if (n < 0)
		return VFS_ERR_INVAL;

	size_t total = (size_t)n;
	if (total >= sizeof(tmp))
		total = sizeof(tmp) - 1;

	return copy_text_slice(tmp, total, off, buf, len, done);
}

static int pci_device_vendor_read(void *ctx, uint64_t off, void *buf,
								  size_t len, size_t *done)
{
	if (!ctx)
		return VFS_ERR_INVAL;

	const pci_device_info_t *dev = ctx;

	char tmp[16];
	int n = npf_snprintf(tmp, sizeof(tmp), "%04x\n", dev->vendor_id);
	if (n < 0)
		return VFS_ERR_INVAL;

	size_t total = (size_t)n;
	if (total >= sizeof(tmp))
		total = sizeof(tmp) - 1;

	return copy_text_slice(tmp, total, off, buf, len, done);
}

static int pci_device_config_read(void *ctx, uint64_t off, void *buf,
								  size_t len, size_t *done)
{
	if (done)
		*done = 0;

	if (!ctx || !buf)
		return VFS_ERR_INVAL;

	if (off >= 256 || len == 0)
		return 0;

	const pci_device_info_t *dev = ctx;

	size_t copy = 256 - (size_t)off;
	if (copy > len)
		copy = len;

	for (size_t i = 0; i < copy; i++) {
		((uint8_t *)buf)[i] =
			pci_read8(dev->bus, dev->slot, dev->function, (uint8_t)(off + i));
	}

	if (done)
		*done = copy;

	return 0;
}

static int pci_publish_device(const pci_device_info_t *dev)
{
	char base[64];
	npf_snprintf(base, sizeof(base), "/dev/pci/%02x:%02x.%u", dev->bus,
				 dev->slot, dev->function);

	int r = devfs_mkdir(base, 0755);
	if (r != 0)
		return r;

	char path[96];

	npf_snprintf(path, sizeof(path), "%s/info", base);
	r = devfs_register_chr(path, 0444, pci_device_info_read, NULL, (void *)dev);
	if (r != 0)
		return r;

	npf_snprintf(path, sizeof(path), "%s/vendor", base);
	r = devfs_register_chr(path, 0444, pci_device_vendor_read, NULL,
						   (void *)dev);
	if (r != 0)
		return r;

	npf_snprintf(path, sizeof(path), "%s/config", base);
	return devfs_register_chr(path, 0444, pci_device_config_read, NULL,
							  (void *)dev);
}

static void pci_log_devices(driver_t *driver)
{
	char logbuf[256];

	for (size_t i = 0; i < pci_devices.count && i < PCI_MAX_SNAPSHOT_DEVICES;
		 i++) {
		const pci_device_info_t *dev = &pci_devices.devices[i];

		char vendor_name[96];
		char device_name[128];

		pci_ids_lookup(dev->vendor_id, dev->device_id, vendor_name,
					   sizeof(vendor_name), device_name, sizeof(device_name));

		const char *v = vendor_name[0] ? vendor_name : "Unknown vendor";
		const char *d = device_name[0] ? device_name : "Unknown device";

		npf_snprintf(logbuf, sizeof(logbuf), "[%02x:%02x.%u] %s — %s", dev->bus,
					 dev->slot, dev->function, v, d);

		driver_log(driver, "info", logbuf);
	}
}

static int pci_main(driver_t *driver)
{
	pci_load_ids(driver);
	pci_scan();

	char msg[96];
	npf_snprintf(msg, sizeof(msg), "enumerated %zu PCI device(s)",
				 pci_devices.count);
	driver_log(driver, "info", msg);

	pci_log_devices(driver);

	int r = ipc_endpoint_register("pci", driver->pid, pci_ipc_handler, NULL);
	if (r != 0)
		return r;

	r = devfs_mkdir("/dev/pci", 0755);
	if (r != 0)
		return r;

	r = devfs_register_chr("/dev/pci/devices", 0444, pci_dev_read, NULL, NULL);
	if (r != 0)
		return r;

	for (size_t i = 0; i < pci_devices.count && i < PCI_MAX_SNAPSHOT_DEVICES;
		 i++) {
		r = pci_publish_device(&pci_devices.devices[i]);
		if (r != 0) {
			driver_log(driver, "err", "failed to publish PCI device");
			return r;
		}
	}

	return 0;
}

static const char *const pci_imports[] = {
	"devfs_mkdir",	 "devfs_register_chr",
	"driver_log",	 "ipc_endpoint_register",
	"kfree",		 "kzalloc",
	"memcpy",		 "memset",
	"npf_snprintf_", "vfs_close",
	"vfs_open",		 "vfs_read",
};

const driver_metadata_t lyr_driver_metadata = {
	.magic = DRIVER_MAGIC,
	.abi_version = DRIVER_ABI_VERSION,
	.name = "pci",
	.entry = pci_main,
	.imports = pci_imports,
	.import_count = sizeof(pci_imports) / sizeof(pci_imports[0]),
};