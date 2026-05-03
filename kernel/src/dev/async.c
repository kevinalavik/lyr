#include <dev/async.h>
#include <cpu/instr.h>
#include <sync/spinlock.h>
#include <stdatomic.h>
#include <stdbool.h>

#define ASYNC_IO_QUEUE_SIZE 512
#define ASYNC_IO_DRAIN_HOOKS 8

static spinlock_t async_io_lock = SPINLOCK_INIT;
static spinlock_t async_io_hook_lock = SPINLOCK_INIT;
static async_io_request_t async_io_queue[ASYNC_IO_QUEUE_SIZE];
static size_t async_io_rpos;
static size_t async_io_wpos;
static atomic_bool async_io_ready = false;
static atomic_uint async_io_drain_depth = 0;

typedef struct {
	async_io_drain_hook_t hook;
	void *context;
} async_io_hook_entry_t;

static async_io_hook_entry_t async_io_hooks[ASYNC_IO_DRAIN_HOOKS];

static bool async_io_empty(void)
{
	return async_io_rpos == async_io_wpos;
}

static bool async_io_full(void)
{
	return ((async_io_wpos + 1) % ASYNC_IO_QUEUE_SIZE) == async_io_rpos;
}

static bool async_io_width_valid(async_io_width_t width)
{
	return width == ASYNC_IO_WIDTH_8 || width == ASYNC_IO_WIDTH_16 ||
		   width == ASYNC_IO_WIDTH_32;
}

static int async_io_validate(const async_io_request_t *request)
{
	if (!request || !async_io_width_valid(request->width))
		return ASYNC_IO_ERR_BAD_REQUEST;

	if (request->op != ASYNC_IO_OP_IN && request->op != ASYNC_IO_OP_OUT)
		return ASYNC_IO_ERR_BAD_REQUEST;

	if (request->target != ASYNC_IO_TARGET_PORT &&
		request->target != ASYNC_IO_TARGET_MMIO)
		return ASYNC_IO_ERR_BAD_REQUEST;

	if (request->op == ASYNC_IO_OP_IN && !request->result)
		return ASYNC_IO_ERR_BAD_REQUEST;

	return ASYNC_IO_OK;
}

static uint32_t async_io_read_port(uint16_t port, async_io_width_t width)
{
	switch (width) {
	case ASYNC_IO_WIDTH_8:
		return inb(port);
	case ASYNC_IO_WIDTH_16:
		return inw(port);
	case ASYNC_IO_WIDTH_32:
		return inl(port);
	default:
		return 0;
	}
}

static void async_io_write_port(uint16_t port, async_io_width_t width,
								uint32_t value)
{
	switch (width) {
	case ASYNC_IO_WIDTH_8:
		outb(port, (uint8_t)value);
		break;
	case ASYNC_IO_WIDTH_16:
		outw(port, (uint16_t)value);
		break;
	case ASYNC_IO_WIDTH_32:
		outl(port, value);
		break;
	default:
		break;
	}
}

static uint32_t async_io_read_mmio(uintptr_t address, async_io_width_t width)
{
	switch (width) {
	case ASYNC_IO_WIDTH_8:
		return *(volatile uint8_t *)address;
	case ASYNC_IO_WIDTH_16:
		return *(volatile uint16_t *)address;
	case ASYNC_IO_WIDTH_32:
		return *(volatile uint32_t *)address;
	default:
		return 0;
	}
}

static void async_io_write_mmio(uintptr_t address, async_io_width_t width,
								uint32_t value)
{
	switch (width) {
	case ASYNC_IO_WIDTH_8:
		*(volatile uint8_t *)address = (uint8_t)value;
		break;
	case ASYNC_IO_WIDTH_16:
		*(volatile uint16_t *)address = (uint16_t)value;
		break;
	case ASYNC_IO_WIDTH_32:
		*(volatile uint32_t *)address = value;
		break;
	default:
		break;
	}
}

static void async_io_store_result(async_io_request_t *request, uint32_t value)
{
	switch (request->width) {
	case ASYNC_IO_WIDTH_8:
		*(uint8_t *)request->result = (uint8_t)value;
		break;
	case ASYNC_IO_WIDTH_16:
		*(uint16_t *)request->result = (uint16_t)value;
		break;
	case ASYNC_IO_WIDTH_32:
		*(uint32_t *)request->result = value;
		break;
	default:
		break;
	}
}

static int async_io_execute(async_io_request_t *request)
{
	int status = async_io_validate(request);

	if (status == ASYNC_IO_OK) {
		if (request->op == ASYNC_IO_OP_IN) {
			uint32_t value;
			if (request->target == ASYNC_IO_TARGET_PORT)
				value = async_io_read_port(request->port, request->width);
			else
				value = async_io_read_mmio(request->address, request->width);
			async_io_store_result(request, value);
		} else {
			if (request->target == ASYNC_IO_TARGET_PORT)
				async_io_write_port(request->port, request->width,
									request->value);
			else
				async_io_write_mmio(request->address, request->width,
									request->value);
		}
	}

	if (request->status)
		*request->status = status;
	if (request->done)
		*request->done = 1;
	if (request->complete)
		request->complete(request, request->context);

	return status;
}

void async_io_init(void)
{
	spinlock_acquire(&async_io_lock);
	async_io_rpos = 0;
	async_io_wpos = 0;
	atomic_store_explicit(&async_io_ready, true, memory_order_release);
	spinlock_release(&async_io_lock);
}

int async_io_is_initialized(void)
{
	return atomic_load_explicit(&async_io_ready, memory_order_acquire) ? 1 : 0;
}

void async_io_request_in(async_io_request_t *request, uint16_t port,
						 async_io_width_t width, void *result,
						 volatile int *done, volatile int *status,
						 async_io_callback_t complete, void *context)
{
	if (!request)
		return;

	request->target = ASYNC_IO_TARGET_PORT;
	request->op = ASYNC_IO_OP_IN;
	request->width = width;
	request->port = port;
	request->address = 0;
	request->value = 0;
	request->result = result;
	request->done = done;
	request->status = status;
	request->complete = complete;
	request->context = context;
}

void async_io_request_out(async_io_request_t *request, uint16_t port,
						  async_io_width_t width, uint32_t value,
						  volatile int *done, volatile int *status,
						  async_io_callback_t complete, void *context)
{
	if (!request)
		return;

	request->target = ASYNC_IO_TARGET_PORT;
	request->op = ASYNC_IO_OP_OUT;
	request->width = width;
	request->port = port;
	request->address = 0;
	request->value = value;
	request->result = NULL;
	request->done = done;
	request->status = status;
	request->complete = complete;
	request->context = context;
}

void async_io_request_mmio_in(async_io_request_t *request, uintptr_t address,
							  async_io_width_t width, void *result,
							  volatile int *done, volatile int *status,
							  async_io_callback_t complete, void *context)
{
	if (!request)
		return;

	request->target = ASYNC_IO_TARGET_MMIO;
	request->op = ASYNC_IO_OP_IN;
	request->width = width;
	request->port = 0;
	request->address = address;
	request->value = 0;
	request->result = result;
	request->done = done;
	request->status = status;
	request->complete = complete;
	request->context = context;
}

void async_io_request_mmio_out(async_io_request_t *request, uintptr_t address,
							   async_io_width_t width, uint32_t value,
							   volatile int *done, volatile int *status,
							   async_io_callback_t complete, void *context)
{
	if (!request)
		return;

	request->target = ASYNC_IO_TARGET_MMIO;
	request->op = ASYNC_IO_OP_OUT;
	request->width = width;
	request->port = 0;
	request->address = address;
	request->value = value;
	request->result = NULL;
	request->done = done;
	request->status = status;
	request->complete = complete;
	request->context = context;
}

int async_io_submit(const async_io_request_t *request)
{
	int status = async_io_validate(request);
	if (status != ASYNC_IO_OK)
		return status;

	if (!spinlock_try_acquire(&async_io_lock))
		return ASYNC_IO_ERR_BUSY;

	if (async_io_full()) {
		spinlock_release(&async_io_lock);
		return ASYNC_IO_ERR_FULL;
	}

	async_io_queue[async_io_wpos] = *request;
	async_io_wpos = (async_io_wpos + 1) % ASYNC_IO_QUEUE_SIZE;
	spinlock_release(&async_io_lock);
	return ASYNC_IO_OK;
}

int async_io_register_drain_hook(async_io_drain_hook_t hook, void *context)
{
	if (!hook)
		return ASYNC_IO_ERR_BAD_REQUEST;

	spinlock_acquire(&async_io_hook_lock);
	for (size_t i = 0; i < ASYNC_IO_DRAIN_HOOKS; i++) {
		if (async_io_hooks[i].hook == hook &&
			async_io_hooks[i].context == context) {
			spinlock_release(&async_io_hook_lock);
			return ASYNC_IO_OK;
		}
	}

	for (size_t i = 0; i < ASYNC_IO_DRAIN_HOOKS; i++) {
		if (!async_io_hooks[i].hook) {
			async_io_hooks[i].hook = hook;
			async_io_hooks[i].context = context;
			spinlock_release(&async_io_hook_lock);
			return ASYNC_IO_OK;
		}
	}
	spinlock_release(&async_io_hook_lock);
	return ASYNC_IO_ERR_FULL;
}

static size_t async_io_drain_queue(size_t budget)
{
	size_t done = 0;

	while (budget == 0 || done < budget) {
		async_io_request_t request;

		if (!spinlock_try_acquire(&async_io_lock))
			break;

		if (async_io_empty()) {
			spinlock_release(&async_io_lock);
			break;
		}

		request = async_io_queue[async_io_rpos];
		async_io_rpos = (async_io_rpos + 1) % ASYNC_IO_QUEUE_SIZE;
		spinlock_release(&async_io_lock);

		async_io_execute(&request);
		done++;
	}

	return done;
}

static size_t async_io_drain_hooks(size_t budget)
{
	size_t done = 0;
	async_io_hook_entry_t hooks[ASYNC_IO_DRAIN_HOOKS];

	if (!spinlock_try_acquire(&async_io_hook_lock))
		return 0;

	for (size_t i = 0; i < ASYNC_IO_DRAIN_HOOKS; i++)
		hooks[i] = async_io_hooks[i];
	spinlock_release(&async_io_hook_lock);

	for (size_t i = 0; i < ASYNC_IO_DRAIN_HOOKS; i++) {
		if (!hooks[i].hook)
			continue;

		size_t remaining = 0;
		if (budget != 0) {
			if (done >= budget)
				break;
			remaining = budget - done;
		}

		done += hooks[i].hook(remaining, hooks[i].context);
	}

	return done;
}

size_t async_io_drain(size_t budget)
{
	atomic_fetch_add_explicit(&async_io_drain_depth, 1, memory_order_acq_rel);

	size_t done = async_io_drain_queue(budget);
	size_t remaining = 0;

	if (budget != 0) {
		if (done >= budget) {
			atomic_fetch_sub_explicit(&async_io_drain_depth, 1,
									  memory_order_acq_rel);
			return done;
		}
		remaining = budget - done;
	}

	done += async_io_drain_hooks(remaining);
	atomic_fetch_sub_explicit(&async_io_drain_depth, 1, memory_order_acq_rel);
	return done;
}

void async_io_flush(void)
{
	for (;;) {
		bool checked_empty = false;
		bool empty = false;
		size_t drained = async_io_drain(64);

		if (spinlock_try_acquire(&async_io_lock)) {
			empty = async_io_empty();
			checked_empty = true;
			spinlock_release(&async_io_lock);
		}

		if (checked_empty && empty && drained == 0)
			break;

		if (drained == 0)
			break;
	}
}

static int async_io_submit_fire_and_forget(async_io_op_t op, uint16_t port,
										   async_io_width_t width,
										   uint32_t value, void *result)
{
	async_io_request_t request;
	if (op == ASYNC_IO_OP_IN)
		async_io_request_in(&request, port, width, result, NULL, NULL, NULL,
							NULL);
	else
		async_io_request_out(&request, port, width, value, NULL, NULL, NULL,
							 NULL);

	return async_io_submit(&request);
}

int async_io_outb(uint16_t port, uint8_t value)
{
	return async_io_submit_fire_and_forget(ASYNC_IO_OP_OUT, port,
										   ASYNC_IO_WIDTH_8, value, NULL);
}

int async_io_outw(uint16_t port, uint16_t value)
{
	return async_io_submit_fire_and_forget(ASYNC_IO_OP_OUT, port,
										   ASYNC_IO_WIDTH_16, value, NULL);
}

int async_io_outl(uint16_t port, uint32_t value)
{
	return async_io_submit_fire_and_forget(ASYNC_IO_OP_OUT, port,
										   ASYNC_IO_WIDTH_32, value, NULL);
}

int async_io_inb(uint16_t port, uint8_t *result)
{
	return async_io_submit_fire_and_forget(ASYNC_IO_OP_IN, port,
										   ASYNC_IO_WIDTH_8, 0, result);
}

int async_io_inw(uint16_t port, uint16_t *result)
{
	return async_io_submit_fire_and_forget(ASYNC_IO_OP_IN, port,
										   ASYNC_IO_WIDTH_16, 0, result);
}

int async_io_inl(uint16_t port, uint32_t *result)
{
	return async_io_submit_fire_and_forget(ASYNC_IO_OP_IN, port,
										   ASYNC_IO_WIDTH_32, 0, result);
}

static int async_io_sync(async_io_request_t *request)
{
	if (atomic_load_explicit(&async_io_drain_depth, memory_order_acquire) != 0)
		return async_io_execute(request);

	async_io_flush();
	return async_io_execute(request);
}

int async_io_outb_sync(uint16_t port, uint8_t value)
{
	async_io_request_t request;
	async_io_request_out(&request, port, ASYNC_IO_WIDTH_8, value, NULL, NULL,
						 NULL, NULL);
	return async_io_sync(&request);
}

int async_io_outw_sync(uint16_t port, uint16_t value)
{
	async_io_request_t request;
	async_io_request_out(&request, port, ASYNC_IO_WIDTH_16, value, NULL, NULL,
						 NULL, NULL);
	return async_io_sync(&request);
}

int async_io_outl_sync(uint16_t port, uint32_t value)
{
	async_io_request_t request;
	async_io_request_out(&request, port, ASYNC_IO_WIDTH_32, value, NULL, NULL,
						 NULL, NULL);
	return async_io_sync(&request);
}

uint8_t async_io_inb_sync(uint16_t port)
{
	uint8_t result = 0;
	async_io_request_t request;
	async_io_request_in(&request, port, ASYNC_IO_WIDTH_8, &result, NULL, NULL,
						NULL, NULL);
	async_io_sync(&request);
	return result;
}

uint16_t async_io_inw_sync(uint16_t port)
{
	uint16_t result = 0;
	async_io_request_t request;
	async_io_request_in(&request, port, ASYNC_IO_WIDTH_16, &result, NULL, NULL,
						NULL, NULL);
	async_io_sync(&request);
	return result;
}

uint32_t async_io_inl_sync(uint16_t port)
{
	uint32_t result = 0;
	async_io_request_t request;
	async_io_request_in(&request, port, ASYNC_IO_WIDTH_32, &result, NULL, NULL,
						NULL, NULL);
	async_io_sync(&request);
	return result;
}

uint8_t async_io_mmio_read8_sync(uintptr_t address)
{
	uint8_t result = 0;
	async_io_request_t request;
	async_io_request_mmio_in(&request, address, ASYNC_IO_WIDTH_8, &result, NULL,
							 NULL, NULL, NULL);
	async_io_sync(&request);
	return result;
}

uint16_t async_io_mmio_read16_sync(uintptr_t address)
{
	uint16_t result = 0;
	async_io_request_t request;
	async_io_request_mmio_in(&request, address, ASYNC_IO_WIDTH_16, &result,
							 NULL, NULL, NULL, NULL);
	async_io_sync(&request);
	return result;
}

uint32_t async_io_mmio_read32_sync(uintptr_t address)
{
	uint32_t result = 0;
	async_io_request_t request;
	async_io_request_mmio_in(&request, address, ASYNC_IO_WIDTH_32, &result,
							 NULL, NULL, NULL, NULL);
	async_io_sync(&request);
	return result;
}

int async_io_mmio_write8_sync(uintptr_t address, uint8_t value)
{
	async_io_request_t request;
	async_io_request_mmio_out(&request, address, ASYNC_IO_WIDTH_8, value, NULL,
							  NULL, NULL, NULL);
	return async_io_sync(&request);
}

int async_io_mmio_write16_sync(uintptr_t address, uint16_t value)
{
	async_io_request_t request;
	async_io_request_mmio_out(&request, address, ASYNC_IO_WIDTH_16, value, NULL,
							  NULL, NULL, NULL);
	return async_io_sync(&request);
}

int async_io_mmio_write32_sync(uintptr_t address, uint32_t value)
{
	async_io_request_t request;
	async_io_request_mmio_out(&request, address, ASYNC_IO_WIDTH_32, value, NULL,
							  NULL, NULL, NULL);
	return async_io_sync(&request);
}
