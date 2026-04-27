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
#include <mm/heap.h>
#include <mm/vmm.h>
#include <debug/test.h>
#include <sys/acpi.h>
#include <sys/bgrt.h>

/* public variables */
uint64_t _lyr_hhdm_offset = 0;
uint64_t _lyr_kstack_top = 0;
uint64_t _lyr_kvirt = 0;
uint64_t _lyr_kphys = 0;
vas_t *_lyr_kernel_vas = NULL;

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

__attribute__((
	used,
	section(
		".limine_requests"))) volatile struct limine_executable_address_request
	kernel_address_request = { .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
							   .response = 0 };

__attribute__((used,
			   section(".limine_requests"))) volatile struct limine_rsdp_request
	rsdp_request = { .id = LIMINE_RSDP_REQUEST_ID, .response = 0 };

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

void lyr_entry(void)
{
	__asm__ volatile("movq %%rsp, %0" : "=r"(_lyr_kstack_top));
	if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false)
		nointloop();

	uint64_t uart_ret = uart_init();
	log_info("entry", "UART %s", uart_ret == 0 ? "ok" : "not ok");

	assert(framebuffer_request.response != NULL);
	assert(framebuffer_request.response->framebuffer_count >= 1);
	assert(framebuffer_request.response->framebuffers[0] != NULL);
	struct limine_framebuffer *framebuffer =
		framebuffer_request.response->framebuffers[0];

	lyrterm_apply_theme(&lyrterm_theme_nord);
	lyrterm_init(framebuffer);

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

	assert(kernel_address_request.response != NULL);
	_lyr_kvirt = kernel_address_request.response->virtual_base;
	_lyr_kphys = kernel_address_request.response->physical_base;
	paging_init();
	assert(kernel_ptable != NULL);
	paging_test();

	kheap_init();
	log_info("entry", "heap ok");
	heap_test();

	_lyr_kernel_vas = vas_adopt(kernel_ptable);
	log_info("entry", "switched to kernel page table");
	vas_switch(_lyr_kernel_vas);
	log_info("entry", "VMM ok");
	vmm_test(_lyr_kernel_vas);

	assert(rsdp_request.response != NULL);
	acpi_init(rsdp_request.response->address);
	acpi_dump_tables();

	/* done for now */
	log_info("entry", "------------------------------");
	log_info("entry", "lyr kernel v1.0.0 (c) 2026 Kevin Alavik");

	bgrt_init(framebuffer);

	nointloop();
}