#ifndef _LYR_DRIVER_PCI_H
#define _LYR_DRIVER_PCI_H

#include <stddef.h>
#include <stdint.h>

#define PCI_IPC_LIST_DEVICES 1
#define PCI_MAX_SNAPSHOT_DEVICES 64

typedef struct {
	uint8_t bus;
	uint8_t slot;
	uint8_t function;
	uint16_t vendor_id;
	uint16_t device_id;
	uint8_t class_code;
	uint8_t subclass;
	uint8_t prog_if;
	uint8_t revision;
} pci_device_info_t;

typedef struct {
	size_t count;
	pci_device_info_t devices[PCI_MAX_SNAPSHOT_DEVICES];
} pci_device_list_t;

#endif /* _LYR_DRIVER_PCI_H */
