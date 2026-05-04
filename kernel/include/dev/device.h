#ifndef _LYR_DEV_DEVICE_H
#define _LYR_DEV_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#define DEVICE_NAME_MAX 31
#define DEVICE_BUS_NAME_MAX 15

typedef struct device device_t;
typedef struct device_handler device_handler_t;

typedef enum {
	DEVICE_BUS_PCI = 1,
} device_bus_t;

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
} device_pci_info_t;

struct device {
	char name[DEVICE_NAME_MAX + 1];
	device_bus_t bus_type;
	union {
		device_pci_info_t pci;
	};
	void *driver_data;
	device_handler_t *handler;
	device_t *next;
};

struct device_handler {
	char name[DEVICE_NAME_MAX + 1];
	device_bus_t bus_type;
	int (*match)(const device_t *dev, void *ctx);
	int (*probe)(device_t *dev, void *ctx);
	void *ctx;
	device_handler_t *next;
};

int device_system_init(void);
int device_register(device_t *dev);
int device_handler_register(device_handler_t *handler);
device_t *device_first(void);

#endif /* _LYR_DEV_DEVICE_H */
