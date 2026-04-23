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

const char *banner[] = { " _             ___  ____  ",
						 "| |_   _ _ __ / _ \\/ ___| ",
						 "| | | | | '__| | | \\___ \\ ",
						 "| | |_| | |  | |_| |___) |",
						 "|_|\\__, |_|   \\___/|____/ ",
						 "   |___/                  ",
						 NULL };

/*	lyr entry */
void lyr_entry(void)
{
	if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
		nointloop();
	}

	if (uart_init() != 0) {
		nointloop();
	}

	assert(framebuffer_request.response != NULL);
	assert(framebuffer_request.response->framebuffer_count >= 1);
	assert(framebuffer_request.response->framebuffers[0] != NULL);
	struct limine_framebuffer *framebuffer =
		framebuffer_request.response->framebuffers[0];

	/* use another theme, dark is the default */
	lyrterm_apply_theme(&lyrterm_theme_dark);
	lyrterm_init(framebuffer->address, framebuffer->width, framebuffer->height,
				 framebuffer->pitch / 4);

	for (int i = 0; banner[i] != NULL; i++) {
		kprintf("%s\n", banner[i]);
	}
	kprintf("\n");

	const char *reset = "\x1b[0m";
	const char *block = "  ";

	for (int i = 40; i <= 47; i++) {
		kprintf("\x1b[%dm%s%s", i, block, reset);
	}
	kprintf("\n");

	for (int i = 100; i <= 107; i++) {
		kprintf("\x1b[%dm%s%s", i, block, reset);
	}
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

	log_info("entry", "got hhdm offset -> %p", _lyr_hhdm_offset);
	pfndb_init(memmap_request.response);
	log_info("entry", "PFNDB ok");

	pmm_init();
	log_info("entry", "PMM ok");

	void *a = palloc_single();
	void *b = palloc_single();

	uint64_t old_a = (uint64_t)a;
	uint64_t old_b = (uint64_t)b;

	log_info("test", "allocated single physical page @ %p", a);
	log_info("test", "allocated single physical page @ %p", b);

	pfree(a);
	log_info("test", "freed single physical page (%p)", old_a);
	pfree(b);
	log_info("test", "freed single physical page (%p)", old_b);

	log_info("test", "expecting %p in next alloc", old_b);

	void *c = palloc_single();

	log_info("test",
			 "freed prior allocs and allocated single physical page @ %p", c);

	assert((uint64_t)c == old_b);
	pfree(c);

	nointloop();
}