#ifndef _LYR_DEBUG_TEST_H
#define _LYR_DEBUG_TEST_H

#include <mm/heap.h>
#include <mm/pmm.h>
#include <debug/log.h>
#include <debug/assert.h>
#include <mm/pfndb.h>
#include <mm/page.h>
#include <lib/string.h>
#include <sched/sched.h>
#include <cpu/instr.h>
#include <dev/async.h>
#include <dev/pit.h>
#include <dev/block.h>
#include <fs/vfs.h>
#include <fs/pipe.h>
#include <fs/tmpfs.h>
#include <fs/ext2.h>
#include <ipc/ipc.h>
#include <net/net.h>
#include <sys/poll.h>
#include <util/kprintf.h>
#include <sys/syscall.h>

typedef struct {
	uint32_t calls;
	uint32_t notify_calls;
	uint32_t last_type;
	size_t last_in_len;
} ipc_test_state_t;

typedef struct {
	uint32_t value;
	char tag[8];
} ipc_test_payload_t;

static int ipc_test_handler(const ipc_msg_t *msg, void *ctx)
{
	ipc_test_state_t *state = ctx;
	assert(msg);
	assert(state);

	state->calls++;
	state->last_type = msg->type;
	state->last_in_len = msg->in_len;

	if (msg->kind == IPC_MSG_NOTIFY) {
		state->notify_calls++;
		return IPC_OK;
	}

	assert(msg->kind == IPC_MSG_CALL);
	assert(msg->type == 0xC0DEu);
	assert(msg->in);
	assert(msg->in_len == sizeof(ipc_test_payload_t));
	const ipc_test_payload_t *in = msg->in;
	assert(in->value == 0x12345678u);
	assert(memcmp(in->tag, "ipc", 4) == 0);

	if (msg->out) {
		assert(msg->out_len >= sizeof(ipc_test_payload_t));
		ipc_test_payload_t *out = msg->out;
		out->value = 0x87654321u;
		memcpy(out->tag, "pong", 5);
		if (msg->actual)
			*msg->actual = sizeof(*out);
	}

	return IPC_OK;
}

static void ipc_test(void)
{
	log_debug("ipc_test", "starting");

	ipc_test_state_t state = { 0 };
	assert(ipc_endpoint_register(NULL, 0, ipc_test_handler, &state) ==
		   IPC_ERR_INVAL);
	assert(ipc_endpoint_register("", 0, ipc_test_handler, &state) ==
		   IPC_ERR_INVAL);
	assert(ipc_endpoint_register("test.ipc", 1, NULL, &state) == IPC_ERR_INVAL);
	assert(ipc_endpoint_register("test.ipc", 1, ipc_test_handler, &state) ==
		   IPC_OK);
	assert(ipc_endpoint_register("test.ipc", 1, ipc_test_handler, &state) ==
		   IPC_ERR_EXIST);
	vfs_stat_t st;
	assert(vfs_stat("/dev/ipc/test.ipc", &vfs_root_cred, &st) == 0);
	assert(VFS_S_ISCHR(st.mode));
	vfs_file_t *endpoint_file = NULL;
	assert(vfs_open("/dev/ipc/test.ipc", VFS_O_RDONLY, 0, &vfs_root_cred,
					&endpoint_file) == 0);
	char endpoint_info[64];
	size_t endpoint_info_len = 0;
	assert(vfs_read(endpoint_file, endpoint_info, sizeof(endpoint_info) - 1,
					&endpoint_info_len) == 0);
	vfs_close(endpoint_file);
	endpoint_info[endpoint_info_len] = '\0';
	assert(strlen(endpoint_info) > 0);

	ipc_test_payload_t in = { .value = 0x12345678u };
	memcpy(in.tag, "ipc", 4);
	ipc_test_payload_t out = { 0 };
	size_t actual = 0;
	ipc_msg_t msg = {
		.kind = IPC_MSG_CALL,
		.type = 0xC0DEu,
		.in = &in,
		.in_len = sizeof(in),
		.out = &out,
		.out_len = sizeof(out),
		.actual = &actual,
	};

	assert(ipc_call("missing.ipc", &msg) == IPC_ERR_NOENT);
	assert(ipc_call("test.ipc", &msg) == IPC_OK);
	assert(state.calls == 1);
	assert(state.last_type == 0xC0DEu);
	assert(state.last_in_len == sizeof(in));
	assert(actual == sizeof(out));
	assert(out.value == 0x87654321u);
	assert(memcmp(out.tag, "pong", 5) == 0);

	assert(ipc_notify("test.ipc", 0xAA55u, &in, sizeof(in)) == IPC_OK);
	assert(state.calls == 2);
	assert(state.notify_calls == 1);
	assert(state.last_type == 0xAA55u);

	char snapshot[128];
	assert(ipc_snapshot(snapshot, sizeof(snapshot)) == IPC_OK);
	assert(strlen(snapshot) > 0);

	ipc_shm_t *created = NULL;
	assert(ipc_shm_create(NULL, sizeof(in), &created) == IPC_ERR_INVAL);
	assert(ipc_shm_create("test.ipc.shm", sizeof(in), &created) == IPC_OK);
	assert(created);
	assert(created->size == sizeof(in));
	assert(created->data);
	memcpy(created->data, &in, sizeof(in));
	assert(vfs_stat("/dev/shm/test.ipc.shm", &vfs_root_cred, &st) == 0);
	assert(VFS_S_ISCHR(st.mode));

	ipc_shm_t *opened = NULL;
	assert(ipc_shm_open("missing.ipc.shm", &opened) == IPC_ERR_NOENT);
	assert(ipc_shm_open("test.ipc.shm", &opened) == IPC_OK);
	assert(opened == created);
	ipc_test_payload_t *stored = opened->data;
	assert(stored->value == 0x12345678u);
	assert(memcmp(stored->tag, "ipc", 4) == 0);
	vfs_file_t *shm_file = NULL;
	assert(vfs_open("/dev/shm/test.ipc.shm", VFS_O_RDWR, 0, &vfs_root_cred,
					&shm_file) == 0);
	ipc_test_payload_t via_dev = { .value = 0xAABBCCDDu };
	memcpy(via_dev.tag, "dev", 4);
	size_t io_done = 0;
	assert(vfs_write(shm_file, &via_dev, sizeof(via_dev), &io_done) == 0);
	assert(io_done == sizeof(via_dev));
	assert(vfs_seek(shm_file, VFS_SEEK_SET, 0, NULL) == 0);
	memset(&via_dev, 0, sizeof(via_dev));
	assert(vfs_read(shm_file, &via_dev, sizeof(via_dev), &io_done) == 0);
	vfs_close(shm_file);
	assert(io_done == sizeof(via_dev));
	assert(via_dev.value == 0xAABBCCDDu);
	assert(memcmp(via_dev.tag, "dev", 4) == 0);
	assert(ipc_shm_unlink("test.ipc.shm") == IPC_OK);
	assert(vfs_stat("/dev/shm/test.ipc.shm", &vfs_root_cred, &st) ==
		   -ENOENT);
	assert(ipc_endpoint_unregister("test.ipc") == IPC_OK);
	assert(vfs_stat("/dev/ipc/test.ipc", &vfs_root_cred, &st) == -ENOENT);

	log_debug("ipc_test", "all tests passed");
}

typedef struct {
	uint32_t calls;
	int last_status;
	async_io_op_t last_op;
	async_io_target_t last_target;
} async_test_cb_t;

typedef struct {
	size_t remaining;
	size_t calls;
	size_t last_budget;
} async_test_hook_t;

static async_test_hook_t async_test_hook_ctx;

static void async_test_complete(const async_io_request_t *request,
								void *context)
{
	async_test_cb_t *cb = context;
	assert(cb);
	assert(request);

	cb->calls++;
	cb->last_status = request->status ? *request->status : ASYNC_IO_OK;
	cb->last_op = request->op;
	cb->last_target = request->target;
}

static size_t async_test_drain_hook(size_t budget, void *context)
{
	async_test_hook_t *hook = context;
	assert(hook);

	hook->calls++;
	hook->last_budget = budget;

	if (hook->remaining == 0)
		return 0;

	size_t n = hook->remaining;
	if (budget != 0 && n > budget)
		n = budget;

	hook->remaining -= n;
	return n;
}

static void async_test(void)
{
	log_debug("async_test", "starting");
	assert(async_io_is_initialized());

	/* 1. invalid request validation */
	{
		async_io_request_t req;
		uint32_t result = 0;

		assert(async_io_submit(NULL) == ASYNC_IO_ERR_BAD_REQUEST);

		async_io_request_mmio_in(&req, (uintptr_t)&result, ASYNC_IO_WIDTH_32,
								 NULL, NULL, NULL, NULL, NULL);
		assert(async_io_submit(&req) == ASYNC_IO_ERR_BAD_REQUEST);

		async_io_request_mmio_out(&req, (uintptr_t)&result, (async_io_width_t)7,
								  0, NULL, NULL, NULL, NULL);
		assert(async_io_submit(&req) == ASYNC_IO_ERR_BAD_REQUEST);

		assert(async_io_register_drain_hook(NULL, NULL) ==
			   ASYNC_IO_ERR_BAD_REQUEST);
		log_debug("async_test", "validation ok");
	}

	/* 2. queued MMIO write/read, done/status, callback */
	{
		uint32_t mmio = 0;
		uint32_t result = 0;
		volatile int done = 0;
		volatile int status = 123;
		async_test_cb_t cb = { 0 };
		async_io_request_t req;

		async_io_request_mmio_out(&req, (uintptr_t)&mmio, ASYNC_IO_WIDTH_32,
								  0xA5A55A5Au, &done, &status,
								  async_test_complete, &cb);
		assert(async_io_submit(&req) == ASYNC_IO_OK);
		assert(mmio == 0);
		assert(done == 0);

		assert(async_io_drain(1) == 1);
		assert(mmio == 0xA5A55A5Au);
		assert(done == 1);
		assert(status == ASYNC_IO_OK);
		assert(cb.calls == 1);
		assert(cb.last_status == ASYNC_IO_OK);
		assert(cb.last_op == ASYNC_IO_OP_OUT);
		assert(cb.last_target == ASYNC_IO_TARGET_MMIO);

		mmio = 0x11223344u;
		done = 0;
		status = 321;
		cb.calls = 0;
		async_io_request_mmio_in(&req, (uintptr_t)&mmio, ASYNC_IO_WIDTH_32,
								 &result, &done, &status, async_test_complete,
								 &cb);
		assert(async_io_submit(&req) == ASYNC_IO_OK);
		assert(result == 0);
		assert(async_io_drain(1) == 1);
		assert(result == 0x11223344u);
		assert(done == 1);
		assert(status == ASYNC_IO_OK);
		assert(cb.calls == 1);
		assert(cb.last_op == ASYNC_IO_OP_IN);
		log_debug("async_test", "queued mmio read/write ok");
	}

	/* 3. drain budget preserves FIFO ordering */
	{
		uint32_t mmio = 0;
		async_io_request_t req;

		async_io_request_mmio_out(&req, (uintptr_t)&mmio, ASYNC_IO_WIDTH_32, 1,
								  NULL, NULL, NULL, NULL);
		assert(async_io_submit(&req) == ASYNC_IO_OK);
		async_io_request_mmio_out(&req, (uintptr_t)&mmio, ASYNC_IO_WIDTH_32, 2,
								  NULL, NULL, NULL, NULL);
		assert(async_io_submit(&req) == ASYNC_IO_OK);

		assert(async_io_drain(1) == 1);
		assert(mmio == 1);
		assert(async_io_drain(1) == 1);
		assert(mmio == 2);
		assert(async_io_drain(1) == 0);
		log_debug("async_test", "budget/FIFO ok");
	}

	/* 4. sync MMIO helpers for every supported width */
	{
		uint8_t mmio8 = 0;
		uint16_t mmio16 = 0;
		uint32_t mmio32 = 0;

		assert(async_io_mmio_write8_sync((uintptr_t)&mmio8, 0x7Bu) ==
			   ASYNC_IO_OK);
		assert(mmio8 == 0x7Bu);
		mmio8 = 0x5Cu;
		assert(async_io_mmio_read8_sync((uintptr_t)&mmio8) == 0x5Cu);

		assert(async_io_mmio_write16_sync((uintptr_t)&mmio16, 0xBEEFu) ==
			   ASYNC_IO_OK);
		assert(mmio16 == 0xBEEFu);
		mmio16 = 0xCAFEu;
		assert(async_io_mmio_read16_sync((uintptr_t)&mmio16) == 0xCAFEu);

		assert(async_io_mmio_write32_sync((uintptr_t)&mmio32, 0xDEADBEEFu) ==
			   ASYNC_IO_OK);
		assert(mmio32 == 0xDEADBEEFu);
		mmio32 = 0xC001CAFEu;
		assert(async_io_mmio_read32_sync((uintptr_t)&mmio32) == 0xC001CAFEu);
		log_debug("async_test", "sync mmio helpers ok");
	}

	/* 5. drain hooks are idempotent and budgeted */
	{
		async_test_hook_ctx.remaining = 5;
		async_test_hook_ctx.calls = 0;
		async_test_hook_ctx.last_budget = 0;

		assert(async_io_register_drain_hook(
				   async_test_drain_hook, &async_test_hook_ctx) == ASYNC_IO_OK);
		assert(async_io_register_drain_hook(
				   async_test_drain_hook, &async_test_hook_ctx) == ASYNC_IO_OK);

		assert(async_io_drain(3) == 3);
		assert(async_test_hook_ctx.remaining == 2);
		assert(async_test_hook_ctx.last_budget == 3);

		assert(async_io_drain(0) == 2);
		assert(async_test_hook_ctx.remaining == 0);
		log_debug("async_test", "drain hooks ok");
	}

	/* 6. queue full is reported and a full drain executes all requests */
	{
		uint32_t mmio = 0;
		async_io_request_t req;
		size_t queued = 0;

		for (uint32_t i = 0; i < 1024; i++) {
			async_io_request_mmio_out(&req, (uintptr_t)&mmio, ASYNC_IO_WIDTH_32,
									  i, NULL, NULL, NULL, NULL);
			int status = async_io_submit(&req);
			if (status == ASYNC_IO_OK) {
				queued++;
				continue;
			}

			assert(status == ASYNC_IO_ERR_FULL);
			break;
		}

		assert(queued > 0);
		assert(async_io_drain(0) == queued);
		assert(mmio == (uint32_t)(queued - 1));
		assert(async_io_drain(0) == 0);
		log_debug("async_test", "queue full/drain ok queued=%zu", queued);
	}

	log_debug("async_test", "all tests passed");
}

static void pmm_test(void)
{
	/* 1. LIFO order */
	{
		page_t *a = palloc_page();
		page_t *b = palloc_page();
		uint64_t phys_a = pfndb_page_to_phys(a);
		uint64_t phys_b = pfndb_page_to_phys(b);

		page_unref(a);
		page_unref(b);

		page_t *c = palloc_page();
		page_t *d = palloc_page();

		assert(pfndb_page_to_phys(c) == phys_b);
		assert(pfndb_page_to_phys(d) == phys_a);

		page_unref(c);
		page_unref(d);
		log_debug("pmm_test", "LIFO order ok (a=0x%llx b=0x%llx)", phys_a,
				  phys_b);
	}

	/* 2. refcount */
	{
		page_t *p = palloc_page();
		assert(p->refcount == 1);

		page_ref(p);
		assert(p->refcount == 2);
		assert(page_is_used(p));

		page_unref(p);
		assert(p->refcount == 1);
		assert(page_is_used(p));

		page_unref(p);
		assert(p->refcount == 0);
		assert(page_is_free(p));
		log_debug("pmm_test", "refcount ok");
	}

	/* 3. sharecount + PAGE_SHARED flag */
	{
		page_t *p = palloc_page();
		assert(p->u2.sharecount == 0);
		assert(!page_is_shared(p));

		page_share(p);
		assert(p->u2.sharecount == 1);
		assert(!page_is_shared(p));

		page_share(p);
		assert(p->u2.sharecount == 2);
		assert(page_is_shared(p));

		page_unshare(p);
		assert(p->u2.sharecount == 1);
		assert(!page_is_shared(p));

		page_unshare(p);
		assert(p->u2.sharecount == 0);

		page_unref(p);
		log_debug("pmm_test", "sharecount ok");
	}

	/* 4. scrub on free */
	{
		page_t *p = palloc_page();
		char *virt = PHYS_TO_VIRT(pfndb_page_to_phys(p));
		memset(virt, 0xAB, PAGE_SIZE);
		page_unref(p);

		page_t *q = palloc_page();
		assert(pfndb_page_to_phys(q) == pfndb_page_to_phys(p));

		char *virt2 = PHYS_TO_VIRT(pfndb_page_to_phys(q));
		for (int i = 0; i < (int)PAGE_SIZE; i++)
			assert(virt2[i] == 0);

		page_unref(q);
		log_debug("pmm_test", "scrub ok");
	}

	/* 5. HHDM write */
	{
		page_t *p = palloc_page();
		char *virt = PHYS_TO_VIRT(pfndb_page_to_phys(p));
		*virt = 'Z';
		assert(*virt == 'Z');
		assert(p->refcount == 1);
		page_unref(p);
		log_debug("pmm_test", "HHDM write ok");
	}

	log_debug("pmm_test", "all tests passed");
}

static void paging_test(void)
{
	uint64_t cr3 = read_cr3();
	ptable_t *pt = (ptable_t *)cr3;

	/* 1. basic map, write, read, unmap */
	{
		uint64_t virt = 0xdeadbeef000ULL;
		page_t *page = palloc_page();

		map_page(pt, virt, page, VMM_FLAGS_KERNEL_RW);
		assert(page->u2.sharecount == 1);
		assert(page->refcount == 2); /* palloc gave 1, map_page added 1 */

		volatile char *ptr = (volatile char *)virt;
		*ptr = 'A';
		assert(*ptr == 'A');
		log_debug("paging_test",
				  "map+write ok: virt=0x%llx phys=0x%llx val='%c'", virt,
				  pfndb_page_to_phys(page), *ptr);

		unmap_page(pt, virt);
		assert(page->u2.sharecount == 0);
		assert(page->refcount == 1);
		log_debug("paging_test", "unmap ok");

		page_unref(page);
	}

	/* 2. get_phys */
	{
		uint64_t virt = 0xcafe0000ULL;
		page_t *page = palloc_page();
		uint64_t phys = pfndb_page_to_phys(page);

		map_page(pt, virt, page, VMM_FLAGS_KERNEL_RW);
		assert(get_phys(pt, virt) == phys);
		assert(get_phys(pt, virt + 1) == phys + 1);
		log_debug("paging_test", "get_phys ok: 0x%llx -> 0x%llx", virt, phys);

		unmap_page(pt, virt);
		assert(get_phys(pt, virt) == 0);
		page_unref(page);
	}

	/* 3. double-map same page into two virtual addresses */
	{
		uint64_t virt1 = 0xf000000000ULL;
		uint64_t virt2 = 0xf000001000ULL;
		page_t *page = palloc_page();

		map_page(pt, virt1, page, VMM_FLAGS_KERNEL_RW);
		map_page(pt, virt2, page, VMM_FLAGS_KERNEL_RW);
		assert(page->u2.sharecount == 2);
		assert(page_is_shared(page));

		volatile char *p1 = (volatile char *)virt1;
		volatile char *p2 = (volatile char *)virt2;
		*p1 = 'X';
		assert(*p2 == 'X'); /* same physical page, both should read 'X' */
		log_debug("paging_test", "shared mapping ok: virt1=0x%llx virt2=0x%llx",
				  virt1, virt2);

		unmap_page(pt, virt1);
		assert(page->u2.sharecount == 1);
		assert(!page_is_shared(page));

		unmap_page(pt, virt2);
		assert(page->u2.sharecount == 0);
		assert(page->refcount == 1);

		page_unref(page);
	}

	log_debug("paging_test", "all tests passed");
}

static void heap_test(void)
{
	log_debug("heap_test", "starting");

	/* 1. basic alloc/free (small sizes) */
	{
		void *a = kmalloc(16);
		void *b = kmalloc(32);
		assert(a && b);

		memset(a, 0xAA, 16);
		memset(b, 0xBB, 32);

		kfree(a);
		kfree(b);

		log_debug("heap_test", "basic alloc/free ok");
	}

	/* 2. slab reuse */
	{
		void *a = kmalloc(64);
		kfree(a);

		void *b = kmalloc(64);
		assert(b != NULL);

		kfree(b);
		log_debug("heap_test", "slab reuse ok");
	}

	/* 3. fill slab completely */
	{
		size_t sz = 128;
		void *objs[128];

		for (int i = 0; i < 128; i++) {
			objs[i] = kmalloc(sz);
			assert(objs[i]);
		}

		for (int i = 0; i < 128; i++)
			kfree(objs[i]);

		log_debug("heap_test", "slab fill/free ok");
	}

	/* 4. write/read correctness */
	{
		char *p = kmalloc(256);
		assert(p);

		for (int i = 0; i < 256; i++)
			p[i] = (char)i;

		for (int i = 0; i < 256; i++)
			assert(p[i] == (char)i);

		kfree(p);
		log_debug("heap_test", "write/read ok");
	}

	/* 5. large allocation */
	{
		size_t big = PAGE_SIZE * 3;
		void *p = kmalloc(big);
		assert(p);

		memset(p, 0xCC, big);

		for (size_t i = 0; i < big; i++)
			assert(((uint8_t *)p)[i] == 0xCC);

		kfree(p);
		log_debug("heap_test", "large alloc ok");
	}

	/* 6. krealloc grow */
	{
		char *p = kmalloc(64);
		assert(p);

		strcpy(p, "hello world");

		p = krealloc(p, 512);
		assert(p);
		assert(strcmp(p, "hello world") == 0);

		kfree(p);
		log_debug("heap_test", "realloc grow ok");
	}

	/* 7. krealloc shrink */
	{
		char *p = kmalloc(512);
		assert(p);

		strcpy(p, "shrink test");

		p = krealloc(p, 32);
		assert(p);
		assert(strcmp(p, "shrink test") == 0);

		kfree(p);
		log_debug("heap_test", "realloc shrink ok");
	}

	/* 8. kzalloc */
	{
		uint8_t *p = kzalloc(128);
		assert(p);

		for (int i = 0; i < 128; i++)
			assert(p[i] == 0);

		kfree(p);
		log_debug("heap_test", "kzalloc zeroing ok");
	}

	log_debug("heap_test", "all tests passed");
}

static void vmm_test(vas_t *vas)
{
	log_debug("vmm_test", "starting");

	uint64_t free_before = pmm_free_pages();

	/* 1. basic anonymous map, write, unmap */
	{
		uint64_t addr =
			vas_map_anon(vas, 0, PAGE_SIZE, VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);
		log_debug("vmm_test", "1: anon map at 0x%llx", addr);

		vad_t *v = vas_find(vas, addr);
		assert(v != NULL);
		assert(v->start == addr);
		assert(v->end == addr + PAGE_SIZE);
		assert(v->flags & VAD_ANONYMOUS);

		volatile uint64_t *p = (volatile uint64_t *)addr;
		*p = 0xCAFEBABEDEADBEEFull;
		assert(*p == 0xCAFEBABEDEADBEEFull);

		uint64_t phys = get_phys(vas->pml4, addr);
		assert(phys != 0);
		log_debug("vmm_test", "1: phys=0x%llx val=0x%llx ok", phys, *p);

		vas_unmap(vas, addr, PAGE_SIZE);
		assert(vas_find(vas, addr) == NULL);
		assert(get_phys(vas->pml4, addr) == 0);

		log_debug("vmm_test", "1: anon map/write/unmap ok");
	}

	/* 2. multi-page anonymous map */
	{
		size_t len = PAGE_SIZE * 4;
		uint64_t addr = vas_map_anon(vas, 0, len, VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);

		for (size_t off = 0; off < len; off += PAGE_SIZE) {
			volatile uint8_t *p = (volatile uint8_t *)(addr + off);
			*p = (uint8_t)(off / PAGE_SIZE);
			assert(*p == (uint8_t)(off / PAGE_SIZE));
			assert(get_phys(vas->pml4, addr + off) != 0);
		}

		vas_unmap(vas, addr, len);

		for (size_t off = 0; off < len; off += PAGE_SIZE)
			assert(get_phys(vas->pml4, addr + off) == 0);

		log_debug("vmm_test", "2: multi-page anon ok");
	}

	/* 3. VAD_FIXED placement */
	{
		uint64_t want = 0x0000500000000000ull;
		uint64_t addr = vas_map_anon(vas, want, PAGE_SIZE,
									 VMM_PRESENT | VMM_WRITABLE | VAD_FIXED);
		assert(addr == want);

		vad_t *v = vas_find(vas, want);
		assert(v && v->start == want);

		volatile uint32_t *p = (volatile uint32_t *)want;
		*p = 0xABCD1234u;
		assert(*p == 0xABCD1234u);

		vas_unmap(vas, want, PAGE_SIZE);
		assert(vas_find(vas, want) == NULL);

		log_debug("vmm_test", "3: VAD_FIXED ok");
	}

	/* 4. sequential allocations don't overlap */
	{
		uint64_t a =
			vas_map_anon(vas, 0, PAGE_SIZE, VMM_PRESENT | VMM_WRITABLE);
		uint64_t b =
			vas_map_anon(vas, 0, PAGE_SIZE * 2, VMM_PRESENT | VMM_WRITABLE);
		uint64_t c =
			vas_map_anon(vas, 0, PAGE_SIZE, VMM_PRESENT | VMM_WRITABLE);

		assert(a && b && c);
		assert(a != b && b != c && a != c);
		assert(a + PAGE_SIZE <= b || b + PAGE_SIZE * 2 <= a);
		assert(b + PAGE_SIZE * 2 <= c || c + PAGE_SIZE <= b);
		assert(a + PAGE_SIZE <= c || c + PAGE_SIZE <= a);
		log_debug("vmm_test", "4: a=0x%llx b=0x%llx c=0x%llx", a, b, c);

		vas_unmap(vas, a, PAGE_SIZE);
		vas_unmap(vas, b, PAGE_SIZE * 2);
		vas_unmap(vas, c, PAGE_SIZE);

		log_debug("vmm_test", "4: no-overlap ok");
	}

	/* 5. vas_protect strips WRITABLE, data survives */
	{
		uint64_t addr =
			vas_map_anon(vas, 0, PAGE_SIZE, VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);

		volatile uint64_t *p = (volatile uint64_t *)addr;
		*p = 0x1234567890ABCDEFull;
		assert(*p == 0x1234567890ABCDEFull);

		vas_protect(vas, addr, PAGE_SIZE, VMM_PRESENT);

		vad_t *v = vas_find(vas, addr);
		assert(v);
		assert(!(v->flags & VMM_WRITABLE));
		assert(get_phys(vas->pml4, addr) != 0);
		assert(*p == 0x1234567890ABCDEFull);
		log_debug("vmm_test", "5: protect ok, value=0x%llx", *p);

		vas_unmap(vas, addr, PAGE_SIZE);

		log_debug("vmm_test", "5: vas_protect ok");
	}

	/* 6. vas_map_phys — verify same bytes visible through both windows */
	{
		page_t *pg = palloc_page();
		uint64_t phys = pfndb_page_to_phys(pg);

		uint8_t *hhdm = (uint8_t *)PHYS_TO_VIRT(phys);
		for (int i = 0; i < 16; i++)
			hhdm[i] = (uint8_t)(0xA0 + i);

		uint64_t mapped =
			vas_map_phys(vas, 0, phys, PAGE_SIZE, VMM_PRESENT | VMM_WRITABLE);
		assert(mapped != 0);
		log_debug("vmm_test", "6: phys=0x%llx mapped at 0x%llx", phys, mapped);

		uint8_t *via_vas = (uint8_t *)mapped;
		for (int i = 0; i < 16; i++) {
			assert(via_vas[i] == hhdm[i]);
			log_debug("vmm_test", "6: [%d] hhdm=0x%02x vas=0x%02x", i, hhdm[i],
					  via_vas[i]);
		}

		vas_unmap(vas, mapped, PAGE_SIZE);
		assert(get_phys(vas->pml4, mapped) == 0);

		assert(page_is_used(pg));
		page_unref(pg);

		log_debug("vmm_test", "6: vas_map_phys ok");
	}

	/* 7. partial unmap splits VAD into left + right remainders */
	{
		size_t len = PAGE_SIZE * 4;
		uint64_t addr = vas_map_anon(vas, 0, len, VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);

		vas_unmap(vas, addr + PAGE_SIZE, PAGE_SIZE * 2);

		vad_t *left = vas_find(vas, addr);
		assert(left && left->start == addr && left->end == addr + PAGE_SIZE);

		vad_t *right = vas_find(vas, addr + PAGE_SIZE * 3);
		assert(right && right->start == addr + PAGE_SIZE * 3 &&
			   right->end == addr + PAGE_SIZE * 4);

		assert(get_phys(vas->pml4, addr + PAGE_SIZE) == 0);
		assert(get_phys(vas->pml4, addr + PAGE_SIZE * 2) == 0);
		assert(get_phys(vas->pml4, addr) != 0);
		assert(get_phys(vas->pml4, addr + PAGE_SIZE * 3) != 0);

		vas_unmap(vas, addr, PAGE_SIZE);
		vas_unmap(vas, addr + PAGE_SIZE * 3, PAGE_SIZE);

		log_debug("vmm_test", "7: partial unmap / VAD split ok");
	}

	/* 8. pages returned to PMM after full unmap */
	{
		uint64_t snap = pmm_free_pages();
		size_t npages = 8;
		uint64_t addr = vas_map_anon(vas, 0, PAGE_SIZE * npages,
									 VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);

		uint64_t after_map = pmm_free_pages();
		assert(after_map < snap);
		assert(snap - after_map >= npages);

		vas_unmap(vas, addr, PAGE_SIZE * npages);
		assert(pmm_free_pages() == snap);
		log_debug("vmm_test", "8: %llu pages reclaimed ok", (uint64_t)npages);
	}

	/* 9. unique per-word pattern across 32 regions, full read-back */
	{
#define T9_COUNT 32
#define T9_SIZE (PAGE_SIZE * 3)
		void *r[T9_COUNT];
		for (int i = 0; i < T9_COUNT; i++) {
			r[i] = (void *)vas_map_anon(vas, 0, T9_SIZE,
										VMM_PRESENT | VMM_WRITABLE);
			assert(r[i] != NULL);
			uint64_t *p = (uint64_t *)r[i];
			for (size_t j = 0; j < T9_SIZE / sizeof(uint64_t); j++)
				p[j] = 0xABCD000000000000ull ^ ((uint64_t)i << 32) ^ j;
		}
		for (int i = 0; i < T9_COUNT; i++) {
			uint64_t *p = (uint64_t *)r[i];
			for (size_t j = 0; j < T9_SIZE / sizeof(uint64_t); j++) {
				uint64_t expected =
					0xABCD000000000000ull ^ ((uint64_t)i << 32) ^ j;
				assert(p[j] == expected);
			}
			vas_unmap(vas, (uint64_t)r[i], T9_SIZE);
		}
		log_debug("vmm_test", "9: multi-region pattern verify ok");
#undef T9_COUNT
#undef T9_SIZE
	}

	/* 10. interleaved free/realloc — aliasing detection */
	{
#define T10_COUNT 16
#define T10_SIZE PAGE_SIZE
		void *r[T10_COUNT];
		for (int i = 0; i < T10_COUNT; i++) {
			r[i] = (void *)vas_map_anon(vas, 0, T10_SIZE,
										VMM_PRESENT | VMM_WRITABLE);
			assert(r[i] != NULL);
			*(volatile uint64_t *)r[i] = 0xFEEDFACE00000000ull | (uint64_t)i;
		}
		/* free evens, reallocate with inverted sentinel */
		for (int i = 0; i < T10_COUNT; i += 2) {
			vas_unmap(vas, (uint64_t)r[i], T10_SIZE);
			r[i] = (void *)vas_map_anon(vas, 0, T10_SIZE,
										VMM_PRESENT | VMM_WRITABLE);
			assert(r[i] != NULL);
			*(volatile uint64_t *)r[i] = ~(0xFEEDFACE00000000ull | (uint64_t)i);
		}
		/* odd regions must be untouched */
		for (int i = 1; i < T10_COUNT; i += 2) {
			uint64_t expected = 0xFEEDFACE00000000ull | (uint64_t)i;
			assert(*(volatile uint64_t *)r[i] == expected);
		}
		for (int i = 0; i < T10_COUNT; i++)
			vas_unmap(vas, (uint64_t)r[i], T10_SIZE);
		log_debug("vmm_test", "10: interleaved free/realloc aliasing ok");
#undef T10_COUNT
#undef T10_SIZE
	}

	/* 11. vas_protect RW->RO->RW round-trip, data intact */
	{
		uint64_t addr =
			vas_map_anon(vas, 0, PAGE_SIZE * 2, VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);

		uint64_t *p = (uint64_t *)addr;
		size_t words = (PAGE_SIZE * 2) / sizeof(uint64_t);
		for (size_t j = 0; j < words; j++)
			p[j] = 0xC0FFEE00ull ^ j;

		vas_protect(vas, addr, PAGE_SIZE * 2, VMM_PRESENT);
		vas_protect(vas, addr, PAGE_SIZE * 2, VMM_PRESENT | VMM_WRITABLE);

		for (size_t j = 0; j < words; j++)
			assert(p[j] == (0xC0FFEE00ull ^ j));

		vad_t *v = vas_find(vas, addr);
		assert(v && (v->flags & VMM_WRITABLE));
		assert(get_phys(vas->pml4, addr) != 0);
		assert(get_phys(vas->pml4, addr + PAGE_SIZE) != 0);

		vas_unmap(vas, addr, PAGE_SIZE * 2);
		log_debug("vmm_test", "11: protect round-trip ok");
	}

	/* 12. double-unmap must not crash or corrupt VAD list */
	{
		uint64_t addr =
			vas_map_anon(vas, 0, PAGE_SIZE, VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);
		vas_unmap(vas, addr, PAGE_SIZE);
		vas_unmap(vas, addr, PAGE_SIZE); /* no-op, must not panic */
		assert(vas_find(vas, addr) == NULL);
		log_debug("vmm_test", "12: double-unmap ok");
	}

	/* 13. zero-length map must fail; size-1 must round up and succeed */
	{
		uint64_t r = vas_map_anon(vas, 0, 0, VMM_PRESENT | VMM_WRITABLE);
		assert(r == 0);

		uint64_t addr = vas_map_anon(vas, 0, 1, VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);
		assert((addr & 0xFFF) == 0);
		*(volatile uint8_t *)addr = 0xBE;
		assert(*(volatile uint8_t *)addr == 0xBE);
		vas_unmap(vas, addr, 1);
		log_debug("vmm_test", "13: zero-len fail / sub-page round-up ok");
	}

	/* 14. VAD_FIXED collision must fail */
	{
		uint64_t want = 0x0000480000000000ull;
		uint64_t first = vas_map_anon(vas, want, PAGE_SIZE,
									  VMM_PRESENT | VMM_WRITABLE | VAD_FIXED);
		assert(first == want);

		uint64_t collision = vas_map_anon(
			vas, want, PAGE_SIZE, VMM_PRESENT | VMM_WRITABLE | VAD_FIXED);
		assert(collision == 0);

		vas_unmap(vas, want, PAGE_SIZE);
		log_debug("vmm_test", "14: VAD_FIXED collision rejected ok");
	}

	/* 15. PMM leak check — net pages consumed must be zero */
	{
		uint64_t free_after = pmm_free_pages();
		log_debug("vmm_test",
				  "15: free pages before=%llu after=%llu delta=%lld",
				  free_before, free_after,
				  (int64_t)free_after - (int64_t)free_before);
		assert(free_after == free_before);
		log_debug("vmm_test", "15: no PMM leak ok");
	}

	log_debug("vmm_test", "all tests passed");
}

static void vfs_tmpfs_test(vas_t *vas)
{
	log_debug("vfs_tmpfs_test", "starting");

	vfs_cred_t user = { .uid = 1000, .gid = 1000, .umask = 0022 };
	vfs_cred_t other = { .uid = 2000, .gid = 2000, .umask = 0022 };

	assert(vfs_mkdir("/tmp", 0777, &vfs_root_cred) == 0);
	assert(vfs_chmod("/tmp", 01777, &vfs_root_cred) == 0);

	vfs_file_t *file = NULL;
	assert(vfs_open("/tmp/hello", VFS_O_CREAT | VFS_O_RDWR, 0640, &user,
					&file) == 0);
	assert(file);

	const char *msg = "hello tmpfs";
	size_t done = 0;
	assert(vfs_write(file, msg, strlen(msg), &done) == 0);
	assert(done == strlen(msg));
	assert(vfs_seek(file, VFS_SEEK_SET, 0, NULL) == 0);

	char buf[32];
	memset(buf, 0, sizeof(buf));
	assert(vfs_read(file, buf, sizeof(buf), &done) == 0);
	assert(done == strlen(msg));
	assert(strcmp(buf, msg) == 0);

	vfs_stat_t st;
	assert(vfs_stat("/tmp/hello", &user, &st) == 0);
	assert(VFS_S_ISREG(st.mode));
	assert((st.mode & 0777) == 0640);
	assert(st.uid == user.uid);
	assert(st.gid == user.gid);
	assert(st.size == strlen(msg));
	log_debug("vfs_tmpfs_test", "create/read/write/stat ok");

	vfs_file_t *denied = NULL;
	assert(vfs_open("/tmp/hello", VFS_O_RDONLY, 0, &other, &denied) ==
		   -EACCES);
	assert(vfs_chown("/tmp/hello", other.uid, other.gid, &user) ==
		   -EPERM);
	assert(vfs_unlink("/tmp/hello", &other) == -EPERM);
	log_debug("vfs_tmpfs_test", "permissions/sticky directory ok");

	uint64_t map = vas_map_file(vas, 0, vfs_file_node(file), 0, PAGE_SIZE,
								VMM_PRESENT | VMM_WRITABLE | VAD_SHARED);
	assert(map != 0);
	char *mapped = (char *)map;
	assert(mapped[0] == 'h');
	mapped[0] = 'H';
	mapped[6] = 'T';

	assert(vfs_seek(file, VFS_SEEK_SET, 0, NULL) == 0);
	memset(buf, 0, sizeof(buf));
	assert(vfs_read(file, buf, strlen(msg), &done) == 0);
	assert(done == strlen(msg));
	assert(strcmp(buf, "Hello Tmpfs") == 0);
	vas_unmap(vas, map, PAGE_SIZE);
	log_debug("vfs_tmpfs_test", "file-backed VMM mapping ok");

	vfs_file_t *pipe_read = NULL;
	vfs_file_t *pipe_write = NULL;
	assert(vfs_pipe_create(&pipe_read, &pipe_write) == 0);
	assert(pipe_read);
	assert(pipe_write);

	const char *pipe_msg = "abcdef";
	assert(vfs_pipe_write(pipe_write, pipe_msg, strlen(pipe_msg), &done) == 0);
	assert(done == strlen(pipe_msg));
	memset(buf, 0, sizeof(buf));
	assert(vfs_seek(pipe_read, VFS_SEEK_SET, 2, NULL) == 0);
	assert(vfs_pipe_read(pipe_read, buf, 3, &done) == 0);
	assert(done == 3);
	assert(memcmp(buf, "cde", 3) == 0);
	assert(vfs_seek(pipe_read, VFS_SEEK_SET, 0, NULL) == 0);
	memset(buf, 0, sizeof(buf));
	assert(vfs_pipe_read(pipe_read, buf, 6, &done) == 0);
	assert(done == 6);
	assert(memcmp(buf, "abcdef", 6) == 0);
	assert(vfs_seek(pipe_write, VFS_SEEK_END, 0, NULL) == 0);
	assert(vfs_seek(pipe_write, VFS_SEEK_SET, 2, NULL) == 0);
	assert(vfs_pipe_write(pipe_write, "ZZ", 2, &done) == 0);
	assert(done == 2);
	assert(vfs_seek(pipe_read, VFS_SEEK_SET, 0, NULL) == 0);
	memset(buf, 0, sizeof(buf));
	assert(vfs_pipe_read(pipe_read, buf, 6, &done) == 0);
	assert(done == 6);
	assert(memcmp(buf, "abZZef", 6) == 0);
	assert(vfs_poll(pipe_read, LYR_POLLIN | LYR_POLLRDNORM) == 0);
	assert(vfs_poll(pipe_write, LYR_POLLOUT | LYR_POLLWRNORM) ==
		   (LYR_POLLOUT | LYR_POLLWRNORM));
	assert(vfs_close(pipe_write) == 0);
	assert(vfs_poll(pipe_read, LYR_POLLIN | LYR_POLLRDNORM) &
		   LYR_POLLHUP);
	assert(vfs_close(pipe_read) == 0);
	log_debug("vfs_tmpfs_test", "pipe lseek semantics ok");

	assert(vfs_close(file) == 0);
	assert(vfs_unlink("/tmp/hello", &vfs_root_cred) == 0);
	assert(vfs_rmdir("/tmp", &vfs_root_cred) == 0);

	log_debug("vfs_tmpfs_test", "all tests passed");
}

typedef struct {
	atomic_uint *counter;
	atomic_uint *done;
	atomic_bool *start;
	uint32_t iterations;
} sched_test_arg_t;

static void sched_test_thread(void *arg)
{
	sched_test_arg_t *test = arg;
	while (!atomic_load_explicit(test->start, memory_order_acquire))
		__asm__ volatile("pause" ::: "memory");

	for (uint32_t i = 0; i < test->iterations; i++)
		atomic_fetch_add_explicit(test->counter, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(test->done, 1, memory_order_release);
}

static void sched_test_wait_for_start(void *arg)
{
	atomic_bool *start = arg;
	while (!atomic_load_explicit(start, memory_order_acquire))
		__asm__ volatile("pause" ::: "memory");
}

typedef struct {
	uint64_t rip;
	uint64_t rsp;
} sched_user_arg_t;

static void sched_user_launcher(void *arg)
{
	sched_user_arg_t *user = arg;
	sched_enter_user(user->rip, user->rsp);
}

static void sched_write_imm64(uint8_t *p, uint64_t value)
{
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t)(value >> (i * 8));
}

static void sched_test_wait_cleanup(const char *stage)
{
	uint64_t deadline = pit_get_ticks() + 2000;
	while (sched_reap_pending() && pit_get_ticks() < deadline) {
		hlt();
		for (uint32_t i = 0; i < 1000 && sched_reap_pending(); i++)
			__asm__ volatile("pause" ::: "memory");
	}

	assert(!sched_reap_pending());
	log_debug("sched_test", "%s cleanup ok", stage);
}

static void sched_test(void)
{
	log_debug("sched_test", "starting");
	assert(sched_is_initialized());

	tcb_t *current = sched_current();
	assert(current);
	pcb_t *kernel = current->process;
	assert(kernel);
	assert(kernel->pid == 1);
	assert(current->tid == 1);
	assert(kernel->pml4 == kernel_ptable);
	assert(kernel->vas == _lyr_kernel_vas);
	for (uint32_t i = 0; i < cpu_count; i++) {
		assert(cpu_locals[i].idle_thread);
		assert(cpu_locals[i].idle_thread->tid == 0);
		assert(cpu_locals[i].idle_thread->process);
		assert(cpu_locals[i].idle_thread->process->pid == 0);
	}
	log_debug("sched_test", "kernel pcb/current tcb ok");

	pcb_t *proc = sched_process_create("sched-test", NULL);
	assert(proc);
	pid_t placement_pid = proc->pid;
	assert(proc->pid > kernel->pid);
	assert(proc->vas);
	assert(proc->pml4 == proc->vas->pml4);
	assert(proc->pml4 != kernel_ptable);
	log_debug("sched_test", "process create ok pid=%d pml4=0x%llx", proc->pid,
			  (uint64_t)proc->pml4);

	{
		unsigned before[MAX_CPUS];
		atomic_bool placement_start;
		atomic_init(&placement_start, false);

		for (uint32_t i = 0; i < cpu_count; i++)
			before[i] = atomic_load(&cpu_locals[i].sched_load);

		tcb_t *t =
			sched_create_thread(proc, "sched-placement",
								sched_test_wait_for_start, &placement_start);
		assert(t);
		assert(t->process == proc);
		assert(t->mode == TCB_MODE_KERNEL);
		assert(t->cpu);
		tid_t placement_tid = t->tid;
		uint32_t placement_cpu = t->cpu->cpu_index;

		for (uint32_t i = 0; i < cpu_count; i++) {
			if (i == placement_cpu)
				continue;
			assert(before[placement_cpu] <= before[i]);
		}

		log_debug("sched_test", "least-loaded CPU placement ok: tid=%d cpu%u",
				  placement_tid, placement_cpu);
		atomic_store_explicit(&placement_start, true, memory_order_release);
	}

	{
		uint64_t deadline = pit_get_ticks() + 2000;
		while (sched_process_exists(placement_pid) &&
			   pit_get_ticks() < deadline)
			hlt();
		assert(!sched_process_exists(placement_pid));
		sched_test_wait_cleanup("placement");
	}

	{
		enum { THREADS = 4, ITERS = 128 };
		atomic_uint counter;
		atomic_uint done;
		atomic_bool start;
		sched_test_arg_t args[THREADS];
		tcb_t *threads[THREADS];
		bool used[MAX_CPUS];
		uint32_t used_count = 0;

		atomic_init(&counter, 0);
		atomic_init(&done, 0);
		atomic_init(&start, false);
		memset(used, 0, sizeof(used));

		for (int i = 0; i < THREADS; i++) {
			args[i].counter = &counter;
			args[i].done = &done;
			args[i].start = &start;
			args[i].iterations = ITERS;
			threads[i] = sched_create_thread(kernel, "sched-counter",
											 sched_test_thread, &args[i]);
			assert(threads[i] && threads[i]->cpu);
			if (!used[threads[i]->cpu->cpu_index]) {
				used[threads[i]->cpu->cpu_index] = true;
				used_count++;
			}
		}

		atomic_store_explicit(&start, true, memory_order_release);

		uint64_t deadline = pit_get_ticks() + 2000;
		while (atomic_load_explicit(&done, memory_order_acquire) < THREADS &&
			   pit_get_ticks() < deadline)
			hlt();

		assert(atomic_load(&done) == THREADS);
		assert(atomic_load(&counter) == THREADS * ITERS);
		assert(used_count > 1 || cpu_count == 1);
		log_debug("sched_test",
				  "preemptive counter ok: %u increments across %u cpu(s)",
				  atomic_load(&counter), used_count);
		sched_test_wait_cleanup("counter");
	}

	{
		const uint64_t code_va = 0x0000000000400000ULL;
		const uint64_t data_va = 0x0000000000401000ULL;
		const uint64_t stack_va = 0x0000000000402000ULL;
		sched_user_arg_t user_arg;
		pcb_t *uproc = sched_process_create("sched-user-test", NULL);
		assert(uproc);
		pid_t user_pid = uproc->pid;
		page_t *data_page = palloc_page();
		uint64_t data_phys = pfndb_page_to_phys(data_page);
		assert(vas_map_anon(uproc->vas, code_va, PAGE_SIZE,
							VMM_PRESENT | VMM_WRITABLE | VMM_USER |
								VAD_FIXED) == code_va);
		assert(vas_map_phys(uproc->vas, data_va, data_phys, PAGE_SIZE,
							VMM_PRESENT | VMM_WRITABLE | VMM_USER |
								VAD_FIXED) == data_va);
		assert(vas_map_anon(uproc->vas, stack_va, PAGE_SIZE,
							VMM_PRESENT | VMM_WRITABLE | VMM_USER |
								VAD_FIXED) == stack_va);

		uint8_t *code = PHYS_TO_VIRT(get_phys(uproc->pml4, code_va));
		uint64_t *user_counter = PHYS_TO_VIRT(data_phys);
		assert(code && user_counter);
		*user_counter = 0;

		uint8_t program[] = {
			0x48, 0xB8, 0,	  0,		  0, 0, 0,
			0,	  0,	0, /* mov rax, data_va */
			0xF0, 0x48, 0xFF, 0x00, /* lock inc qword [rax] */
			0x48, 0xC7, 0xC0, (SYS_EXIT), 0, 0, 0, /* mov rax, SYS_EXIT */
			0x48, 0x31, 0xFF, /* xor rdi, rdi */
			0x0F, 0x05, /* syscall */
			0xEB, 0xFE, /* jmp $ */
		};
		sched_write_imm64(&program[2], data_va);
		memcpy(code, program, sizeof(program));
		vas_protect(uproc->vas, code_va, PAGE_SIZE, VMM_PRESENT | VMM_USER);

		user_arg.rip = code_va;
		user_arg.rsp = stack_va + PAGE_SIZE - 16;
		tcb_t *ut = sched_create_thread(uproc, "sched-user-counter",
										sched_user_launcher, &user_arg);
		assert(ut);
		uint32_t user_cpu = ut->cpu ? ut->cpu->cpu_index : 0;

		uint64_t deadline = pit_get_ticks() + 2000;
		while ((*user_counter != 1 || sched_process_exists(user_pid) ||
				sched_reap_pending()) &&
			   pit_get_ticks() < deadline)
			hlt();

		assert(*user_counter == 1);
		assert(!sched_process_exists(user_pid));
		assert(!sched_reap_pending());
		page_unref(data_page);
		log_debug("sched_test", "userspace syscall exit ok on cpu%u", user_cpu);
	}

	log_debug("sched_test", "all tests passed");
}


#if _DEBUG_INIT
#define INIT_SMOKE_LOG(fmt, ...) \
	kprintf("\x1b[38;2;120;120;120minit-test: " fmt "\x1b[0m", ##__VA_ARGS__)

static int init_smoke_join_path(const char *parent, const char *name, char *out,
								size_t out_len)
{
	size_t parent_len = strlen(parent);
	size_t name_len = strlen(name);
	int need_slash = !(parent_len == 1 && parent[0] == '/');
	size_t total = parent_len + (need_slash ? 1 : 0) + name_len;
	if (total + 1 > out_len)
		return -ENAMETOOLONG;

	memcpy(out, parent, parent_len);
	size_t pos = parent_len;
	if (need_slash)
		out[pos++] = '/';
	memcpy(out + pos, name, name_len);
	out[pos + name_len] = '\0';
	return 0;
}

static char init_smoke_type_char(vfs_mode_t mode)
{
	if (VFS_S_ISDIR(mode))
		return 'd';
	if (VFS_S_ISCHR(mode))
		return 'c';
	if (VFS_S_ISREG(mode))
		return '-';
	return '?';
}

static void init_smoke_mode_string(vfs_mode_t mode, char out[11])
{
	out[0] = init_smoke_type_char(mode);
	out[1] = (mode & VFS_S_IRUSR) ? 'r' : '-';
	out[2] = (mode & VFS_S_IWUSR) ? 'w' : '-';
	out[3] = (mode & VFS_S_IXUSR) ? 'x' : '-';
	out[4] = (mode & VFS_S_IRGRP) ? 'r' : '-';
	out[5] = (mode & VFS_S_IWGRP) ? 'w' : '-';
	out[6] = (mode & VFS_S_IXGRP) ? 'x' : '-';
	out[7] = (mode & VFS_S_IROTH) ? 'r' : '-';
	out[8] = (mode & VFS_S_IWOTH) ? 'w' : '-';
	out[9] = (mode & VFS_S_IXOTH) ? 'x' : '-';
	out[10] = '\0';
}

static void init_smoke_list_recursive(const char *path)
{
	vfs_stat_t st;
	int r = vfs_stat(path, &vfs_root_cred, &st);
	if (r != 0) {
		kprintf("? %s status=%s(%d)\n", path, errno_name(r), r);
		return;
	}

	char mode[11];
	init_smoke_mode_string(st.mode, mode);
	kprintf("%-10s %3u %5u:%-5u %10llu %04o %s\n", mode, st.nlink, st.uid,
			st.gid, st.size, st.mode & VFS_S_PERM, path);

	if (!VFS_S_ISDIR(st.mode))
		return;

	vfs_node_t *dir = NULL;
	r = vfs_resolve(path, &vfs_root_cred, &dir);
	if (r != 0)
		return;

	for (size_t i = 0;; i++) {
		vfs_dirent_t ent;
		r = vfs_readdir(dir, i, &ent);
		if (r == -ENOENT)
			break;
		if (r != 0)
			break;
		if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0)
			continue;

		char child_path[256];
		r = init_smoke_join_path(path, ent.name, child_path,
								 sizeof(child_path));
		if (r == 0)
			init_smoke_list_recursive(child_path);
	}

	vfs_node_release(dir);
}

static void init_smoke_cat(const char *path)
{
	vfs_file_t *file = NULL;
	int r = vfs_open(path, VFS_O_RDONLY, 0, &vfs_root_cred, &file);
	if (r != 0) {
		INIT_SMOKE_LOG("cat %s failed status=%s(%d)\n", path, errno_name(r),
					   r);
		return;
	}

	size_t cap = file->node->size ? file->node->size : 4096;
	char *buf = kzalloc(cap + 1);
	if (!buf) {
		vfs_close(file);
		return;
	}

	size_t done = 0;
	r = vfs_read(file, buf, cap, &done);
	vfs_close(file);
	if (r == 0) {
		buf[done] = '\0';
		kprintf("%s", buf);
	}
	kfree(buf);
}

static void init_smoke_print_ip_addr(void)
{
	size_t idx = 1;
	for (netdev_t *dev = net_first_dev(); dev; dev = dev->next, idx++) {
		uint32_t broadcast = dev->ipv4_addr | ~dev->ipv4_netmask;
		char ip[24];
		char brd[24];
		net_ipv4_format(dev->ipv4_addr, ip, sizeof(ip));
		net_ipv4_format(broadcast, brd, sizeof(brd));

		kprintf("%zu: %s: <%s,BROADCAST,MULTICAST> mtu %u\n", idx, dev->name,
				dev->link_up ? "UP,LOWER_UP" : "DOWN", dev->mtu);
		if (dev->ipv4_addr)
			kprintf("    inet %s brd %s scope global %s\n", ip, brd, dev->name);
	}
}

static void init_smoke_mount_disk(void)
{
	block_device_t *disk = block_find("nvme0n1");
	if (!disk) {
		INIT_SMOKE_LOG("mount /dev/nvme0n1 on /mnt: no block device\n");
		return;
	}
	int r = ext2_mount(disk, "/mnt");
	INIT_SMOKE_LOG("mount /dev/nvme0n1 on /mnt type ext2 status=%s(%d)\n",
				   errno_name(r), r);
}

static void init_smoke_test(void)
{
	INIT_SMOKE_LOG("banner\n");
	init_smoke_cat("/etc/banner");
	kprintf("\n");

	INIT_SMOKE_LOG("motd\n");
	init_smoke_cat("/etc/motd");

	INIT_SMOKE_LOG("block devices\n");
	init_smoke_mount_disk();
	init_smoke_cat("/dev/mounts");

	INIT_SMOKE_LOG("netdevs\n");
	init_smoke_cat("/dev/net/devices");
	init_smoke_cat("/dev/net/routes");
	init_smoke_print_ip_addr();

	INIT_SMOKE_LOG("filesystem tree\n");
	kprintf("%-10s %3s %11s %10s %4s %s\n", "mode", "lnk", "uid:gid", "size",
			"perm", "path");
	init_smoke_list_recursive("/");
}

#endif

#endif // _LYR_DEBUG_TEST_H
