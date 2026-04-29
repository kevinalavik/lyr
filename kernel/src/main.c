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
#include <sys/acpi/bgrt.h>
#include <sys/acpi/madt.h>
#include <sys/apic.h>
#include <dev/pit.h>
#include <sys/smp.h>

#ifndef LYR_VERSION
#define LYR_VERSION "unknown"
#endif // LYR_VERSION

uint64_t _lyr_hhdm_offset = 0;
uint64_t _lyr_kstack_top = 0;
uint64_t _lyr_kvirt = 0;
uint64_t _lyr_kphys = 0;
vas_t *_lyr_kernel_vas = NULL;
struct limine_bootloader_info_response *_lyr_bootloader_info = NULL;
struct limine_firmware_type_response *_lyr_firmware_type_info = NULL;
struct limine_file *_lyr_file_info = NULL;

#define LIMINE_REQUEST_SECTION \
	__attribute__((used, section(".limine_requests")))

#define LIMINE_REQUEST(type, id_token, name, ...)                         \
	LIMINE_REQUEST_SECTION static volatile struct limine_##type##_request \
		name = { .id = LIMINE_##id_token##_REQUEST_ID,                    \
				 .revision = 0,                                           \
				 ##__VA_ARGS__ }

#define LIMINE_RESPONSE(name) ((name).response)
#define LIMINE_REQUIRE(name) assert(LIMINE_RESPONSE(name) != NULL)

LIMINE_REQUEST_SECTION static volatile uint64_t limine_base_revision[] =
	LIMINE_BASE_REVISION(6);

LIMINE_REQUEST(framebuffer, FRAMEBUFFER, framebuffer_request);
LIMINE_REQUEST(memmap, MEMMAP, memmap_request);
LIMINE_REQUEST(hhdm, HHDM, hhdm_request);
LIMINE_REQUEST(executable_address, EXECUTABLE_ADDRESS, kernel_address_request);
LIMINE_REQUEST(rsdp, RSDP, rsdp_request);
LIMINE_REQUEST(bootloader_info, BOOTLOADER_INFO, bootloader_info_request);
LIMINE_REQUEST(firmware_type, FIRMWARE_TYPE, firmware_type_request);
LIMINE_REQUEST(executable_file, EXECUTABLE_FILE, kernel_file_request);
LIMINE_REQUEST(mp, MP, mp_request);

LIMINE_REQUEST_SECTION static volatile struct limine_stack_size_request
	stack_size_request = { .id = LIMINE_STACK_SIZE_REQUEST_ID,
						   .revision = 0,
						   .stack_size = 64 * 1024ULL };

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
								"   |___/ lyr kernel v" LYR_VERSION
								" (c) 2026 Kevin Alavik",
								NULL };

static void print_banner(void)
{
	for (int i = 0; banner[i]; i++)
		kprintf("\e[0;%dm%s\e[0m\n", 94 + i, banner[i]);
	kprintf("\n");

	/* cool ansi color debug */
	// for (int i = 40; i <= 47; i++)
	// 	kprintf("\x1b[%dm  \x1b[0m", i);
	// kprintf("\n");
	// for (int i = 100; i <= 107; i++)
	// 	kprintf("\x1b[%dm  \x1b[0m", i);
	// kprintf("\n\n");
}

static const char *firmware_type_str(uint32_t type)
{
	switch (type) {
	case LIMINE_FIRMWARE_TYPE_X86BIOS:
		return "BIOS";
	case LIMINE_FIRMWARE_TYPE_EFI64:
		return "UEFI";
	default:
		return "unknown";
	}
}

void lyr_entry(void)
{
	__asm__ volatile("movq %%rsp, %0" : "=r"(_lyr_kstack_top));

	if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision))
		nointloop();

	log_info("entry", "UART %s", uart_init() == 0 ? "ok" : "not ok");

	/* framebuffer */
	LIMINE_REQUIRE(framebuffer_request);
	assert(framebuffer_request.response->framebuffer_count >= 1);
	assert(framebuffer_request.response->framebuffers[0] != NULL);
	struct limine_framebuffer *fb =
		framebuffer_request.response->framebuffers[0];

	lyrterm_apply_theme(&lyrterm_theme_dark);
	lyrterm_init(fb);
	print_banner();

	/* etc requests */
	LIMINE_REQUIRE(bootloader_info_request);
	_lyr_bootloader_info = bootloader_info_request.response;

	LIMINE_REQUIRE(firmware_type_request);
	_lyr_firmware_type_info = firmware_type_request.response;

	LIMINE_REQUIRE(kernel_file_request);
	_lyr_file_info = kernel_file_request.response->executable_file;

	/* gdt and idt */
	gdt_init();
	log_info("entry", "GDT ok");
	idt_init();
	log_info("entry", "IDT ok");

	/* physical memory  */
	LIMINE_REQUIRE(memmap_request);
	assert(memmap_request.response->entry_count != 1);
	assert(memmap_request.response->entries[0] != NULL);
	LIMINE_REQUIRE(hhdm_request);

	_lyr_hhdm_offset = hhdm_request.response->offset;
	log_trace("entry", "HHDM offset -> 0x%llx", _lyr_hhdm_offset);

	pfndb_init(memmap_request.response);
	log_info("entry", "PFNDB ok");
#if _DEBUG
	pfndb_dump();
#endif

	pmm_init();
	log_info("entry", "PMM ok");
	pmm_test();

	/* paging */
	LIMINE_REQUIRE(kernel_address_request);
	_lyr_kvirt = kernel_address_request.response->virtual_base;
	_lyr_kphys = kernel_address_request.response->physical_base;

	paging_init();
	assert(kernel_ptable != NULL);
	paging_test();

	/* heap / VMM */
	kheap_init();
	log_info("entry", "Heap ok");
	heap_test();

	_lyr_kernel_vas = vas_adopt(kernel_ptable);
	log_trace("entry", "switched to kernel page table");
	vas_switch(_lyr_kernel_vas);
	log_info("entry", "VMM ok");
	vmm_test(_lyr_kernel_vas);

	/* ACPI */
	LIMINE_REQUIRE(rsdp_request);
	acpi_init(rsdp_request.response->address);
#if _DEBUG
	acpi_dump_tables();
#endif
	bgrt_init(fb); /* fun thing to test */
	madt_init();
	log_info("entry", "MADT ok");
	apic_init();
	log_info("entry", "APIC ok");

	/* smp */
	LIMINE_REQUIRE(mp_request);
	smp_init(mp_request.response);
	if (get_cpu_local()->lapic_id != 0)
		log_err("entry", "SMP failed");
	else
		log_info("entry", "SMP ok");

	/* timer */
	pit_init(100);

	/* Give the PIT a moment to deliver at least one tick. */
	uint64_t t0 = pit_get_ticks();
	for (uint64_t i = 0; i < 10000000ULL && pit_get_ticks() == t0; i++)
		__asm__ volatile("pause" ::: "memory");

	if (pit_get_ticks() == t0)
		log_err("entry", "PIT (timer) failed");
	else
		log_info("entry", "PIT (timer) ok");

	/* boot summary */
	log_info("entry", "------------------------------");
	log_info("entry", "lyr kernel v" LYR_VERSION " (c) 2026 Kevin Alavik");
	log_info("entry", " * Booted with %s v%s", _lyr_bootloader_info->name,
			 _lyr_bootloader_info->version);
	log_info("entry", " * Firmware type: %s",
			 firmware_type_str(_lyr_firmware_type_info->firmware_type));
	log_info("entry", " * Kernel booted from %s", _lyr_file_info->path);

	for (;;)
		hlt();
}
