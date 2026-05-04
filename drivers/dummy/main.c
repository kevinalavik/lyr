#include <drv/driver.h>
#include <fs/devfs.h>
#include <ipc/ipc.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include "../pci/pci.h"

static pci_device_list_t dummy_last_list;
static size_t dummy_last_bytes;

static int dummy_main(driver_t *driver)
{
	size_t actual = 0;
	pci_device_list_t list;
	ipc_msg_t msg = {
		.kind = IPC_MSG_CALL,
		.type = PCI_IPC_LIST_DEVICES,
		.out = &list,
		.out_len = sizeof(list),
		.actual = &actual,
	};
	int r = ipc_call("pci", &msg);
	if (r != IPC_OK) {
		driver_log(driver, "err", "pci IPC call failed");
		return r;
	}
	if (actual < sizeof(size_t)) {
		driver_log(driver, "err", "pci IPC response was truncated");
		return IPC_ERR_INVAL;
	}
	dummy_last_list = list;
	dummy_last_bytes = actual;

	char logbuf[512];
	for (size_t i = 0; i < list.count && i < PCI_MAX_SNAPSHOT_DEVICES; i++) {
		const pci_device_info_t *dev = &list.devices[i];
		npf_snprintf(
			logbuf, sizeof(logbuf),
			"pci %02x:%02x.%u vendor=%04x device=%04x class=%02x:%02x:%02x rev=%02x",
			dev->bus, dev->slot, dev->function, dev->vendor_id, dev->device_id,
			dev->class_code, dev->subclass, dev->prog_if, dev->revision);
		driver_log(driver, "info", logbuf);
	}

	return r;
}

static const char *const dummy_imports[] = {
	"driver_log",
	"ipc_call",
	"memcpy",
	"npf_snprintf_",
};

const driver_metadata_t lyr_driver_metadata = {
	.magic = DRIVER_MAGIC,
	.abi_version = DRIVER_ABI_VERSION,
	.name = "dummy",
	.entry = dummy_main,
	.imports = dummy_imports,
	.import_count = sizeof(dummy_imports) / sizeof(dummy_imports[0]),
};
