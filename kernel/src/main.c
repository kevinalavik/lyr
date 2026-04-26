#include <limine.h>
#include <cpu/instr.h>
#include <dev/uart.h>
#include <util/kprintf.h>
#include <cpu/gdt.h>
#include <stdbool.h>
#include <lib/lyrterm.h>
#include <debug/log.h>
#include <debug/assert.h>
#include <cpu/idt.h>
#include <debug/panic.h>
#include <mm/pfndb.h>
#include <mm/pmm.h>
#include <mm/page.h>
#include <mm/paging.h>
#include <lib/string.h>

/* public variables */
uint64_t _lyr_hhdm_offset = 0;

/* kernel entry only */
__attribute__((used, section(".limine_requests"))) static volatile uint64_t
	limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((
	used,
	section(
		".limine_requests"))) static volatile struct limine_framebuffer_request
	framebuffer_request = { .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
							.revision = 0 };

__attribute__((
	used,
	section(".limine_requests"))) static volatile struct limine_memmap_request
	memmap_request = { .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0 };

__attribute__((
	used,
	section(".limine_requests"))) static volatile struct limine_hhdm_request
	hhdm_request = { .id = LIMINE_HHDM_REQUEST_ID, .revision = 0 };

__attribute__((used,
			   section(".limine_requests_start"))) static volatile uint64_t
	limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t
	limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

static const char *banner[] = { " _             ___  ____  ",
								"| |_   _ _ __ / _ \\/ ___| ",
								"| | | | | '__| | | \\___ \\ ",
								"| | |_| | |  | |_| |___) |",
								"|_|\\__, |_|   \\___/|____/ ",
								"   |___/                  ",
								NULL };

/* -----------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------- */

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
		log_info("pmm_test", "LIFO order ok (a=0x%llx b=0x%llx)", phys_a,
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
		log_info("pmm_test", "refcount ok");
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
		log_info("pmm_test", "sharecount ok");
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
		log_info("pmm_test", "scrub ok");
	}

	/* 5. HHDM write */
	{
		page_t *p = palloc_page();
		char *virt = PHYS_TO_VIRT(pfndb_page_to_phys(p));
		*virt = 'Z';
		assert(*virt == 'Z');
		assert(p->refcount == 1);
		page_unref(p);
		log_info("pmm_test", "HHDM write ok");
	}

	log_info("pmm_test", "all tests passed");
}

static void paging_test(void)
{
	uint64_t cr3 = read_cr3();
	ptable_t *pt = (ptable_t *)cr3;

	/* 1. basic map, write, read, unmap */
	{
		uint64_t virt = 0xdeadbeef000ULL;
		page_t *page = palloc_page();
		uint64_t phys = pfndb_page_to_phys(page);

		map_page(pt, virt, page, VMM_FLAGS_KERNEL_RW);
		assert(page->u2.sharecount == 1);
		assert(page->refcount == 2); /* palloc gave 1, map_page added 1 */

		volatile char *ptr = (volatile char *)virt;
		*ptr = 'A';
		assert(*ptr == 'A');
		log_info("paging_test",
				 "map+write ok: virt=0x%llx phys=0x%llx val='%c'", virt, phys,
				 *ptr);

		unmap_page(pt, virt);
		assert(page->u2.sharecount == 0);
		assert(page->refcount == 1);
		log_info("paging_test", "unmap ok");

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
		log_info("paging_test", "get_phys ok: 0x%llx -> 0x%llx", virt, phys);

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
		log_info("paging_test", "shared mapping ok: virt1=0x%llx virt2=0x%llx",
				 virt1, virt2);

		unmap_page(pt, virt1);
		assert(page->u2.sharecount == 1);
		assert(!page_is_shared(page));

		unmap_page(pt, virt2);
		assert(page->u2.sharecount == 0);
		assert(page->refcount == 1);

		page_unref(page);
	}

	log_info("paging_test", "all tests passed");
}

void lyr_entry(void)
{
	if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false)
		nointloop();

	if (uart_init() != 0)
		nointloop();

	assert(framebuffer_request.response != NULL);
	assert(framebuffer_request.response->framebuffer_count >= 1);
	assert(framebuffer_request.response->framebuffers[0] != NULL);
	struct limine_framebuffer *framebuffer =
		framebuffer_request.response->framebuffers[0];

	lyrterm_apply_theme(&lyrterm_theme_dark);
	lyrterm_init(framebuffer->address, framebuffer->width, framebuffer->height,
				 framebuffer->pitch / 4);

	for (int i = 0; banner[i] != NULL; i++)
		kprintf("%s\n", banner[i]);
	kprintf("\n");

	const char *reset = "\x1b[0m";
	const char *block = "  ";
	for (int i = 40; i <= 47; i++)
		kprintf("\x1b[%dm%s%s", i, block, reset);
	kprintf("\n");
	for (int i = 100; i <= 107; i++)
		kprintf("\x1b[%dm%s%s", i, block, reset);
	kprintf("\n\n");

	gdt_init();
	log_info("entry", "GDT ok");
	idt_init();
	log_info("entry", "IDT ok");

	assert(memmap_request.response != NULL);
	assert(memmap_request.response->entry_count != 1);
	assert(memmap_request.response->entries[0] != NULL);
	assert(hhdm_request.response != NULL);

	_lyr_hhdm_offset = hhdm_request.response->offset;
	log_info("entry", "HHDM offset -> 0x%llx", _lyr_hhdm_offset);

	pfndb_init(memmap_request.response);
	log_info("entry", "PFNDB ok");
	pfndb_dump();

	pmm_init();
	log_info("entry", "PMM ok");

	pmm_test();
	paging_test();

	nointloop();
}