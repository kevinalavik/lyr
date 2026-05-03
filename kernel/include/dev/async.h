#ifndef _LYR_DEV_ASYNC_H
#define _LYR_DEV_ASYNC_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
	ASYNC_IO_WIDTH_8 = 1,
	ASYNC_IO_WIDTH_16 = 2,
	ASYNC_IO_WIDTH_32 = 4,
} async_io_width_t;

typedef enum {
	ASYNC_IO_OP_IN = 0,
	ASYNC_IO_OP_OUT,
} async_io_op_t;

typedef enum {
	ASYNC_IO_TARGET_PORT = 0,
	ASYNC_IO_TARGET_MMIO,
} async_io_target_t;

typedef struct async_io_request async_io_request_t;
typedef void (*async_io_callback_t)(const async_io_request_t *request,
									void *context);
typedef size_t (*async_io_drain_hook_t)(size_t budget, void *context);

struct async_io_request {
	async_io_target_t target;
	async_io_op_t op;
	async_io_width_t width;
	uint16_t port;
	uintptr_t address;
	uint32_t value;
	void *result;
	volatile int *done;
	volatile int *status;
	async_io_callback_t complete;
	void *context;
};

enum {
	ASYNC_IO_OK = 0,
	ASYNC_IO_ERR_FULL = -1,
	ASYNC_IO_ERR_BAD_REQUEST = -2,
	ASYNC_IO_ERR_BUSY = -3,
};

void async_io_init(void);
int async_io_is_initialized(void);

void async_io_request_in(async_io_request_t *request, uint16_t port,
						 async_io_width_t width, void *result,
						 volatile int *done, volatile int *status,
						 async_io_callback_t complete, void *context);
void async_io_request_out(async_io_request_t *request, uint16_t port,
						  async_io_width_t width, uint32_t value,
						  volatile int *done, volatile int *status,
						  async_io_callback_t complete, void *context);
void async_io_request_mmio_in(async_io_request_t *request, uintptr_t address,
							  async_io_width_t width, void *result,
							  volatile int *done, volatile int *status,
							  async_io_callback_t complete, void *context);
void async_io_request_mmio_out(async_io_request_t *request, uintptr_t address,
							   async_io_width_t width, uint32_t value,
							   volatile int *done, volatile int *status,
							   async_io_callback_t complete, void *context);

int async_io_submit(const async_io_request_t *request);
int async_io_register_drain_hook(async_io_drain_hook_t hook, void *context);
size_t async_io_drain(size_t budget);
void async_io_flush(void);

/*
 * Queued IN requests write into caller-owned storage when drained. Keep the
 * result, done, and status pointers alive until completion/callback.
 */
int async_io_outb(uint16_t port, uint8_t value);
int async_io_outw(uint16_t port, uint16_t value);
int async_io_outl(uint16_t port, uint32_t value);

int async_io_inb(uint16_t port, uint8_t *result);
int async_io_inw(uint16_t port, uint16_t *result);
int async_io_inl(uint16_t port, uint32_t *result);

int async_io_outb_sync(uint16_t port, uint8_t value);
int async_io_outw_sync(uint16_t port, uint16_t value);
int async_io_outl_sync(uint16_t port, uint32_t value);

uint8_t async_io_inb_sync(uint16_t port);
uint16_t async_io_inw_sync(uint16_t port);
uint32_t async_io_inl_sync(uint16_t port);

uint8_t async_io_mmio_read8_sync(uintptr_t address);
uint16_t async_io_mmio_read16_sync(uintptr_t address);
uint32_t async_io_mmio_read32_sync(uintptr_t address);

int async_io_mmio_write8_sync(uintptr_t address, uint8_t value);
int async_io_mmio_write16_sync(uintptr_t address, uint16_t value);
int async_io_mmio_write32_sync(uintptr_t address, uint32_t value);

#endif /* _LYR_DEV_ASYNC_H */
