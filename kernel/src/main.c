#include <limine.h>
#include <cpu/instr.h>
#include <dev/async.h>
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
#include <mm/vmm.h>
#include <debug/test.h>
#include <init/init.h>
#include <sys/acpi.h>
#include <sys/acpi/bgrt.h>
#include <sys/acpi/madt.h>
#include <sys/apic.h>
#include <dev/pit.h>
#include <dev/block.h>
#include <dev/device.h>
#include <dev/time.h>
#include <dev/rtc.h>
#include <sys/smp.h>
#include <sched/sched.h>
#include <fs/initrd.h>
#include <fs/vfs.h>
#include <fs/tmpfs.h>
#include <drv/driver.h>
#include <fs/devfs.h>
#include <ipc/ipc.h>
#include <net/net.h>
#include <net/socket.h>
#include <sys/syscall.h>
#include <dev/console.h>

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
LIMINE_REQUEST(module, MODULE, module_request);
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

static void cpu_enable_sse(void)
{
	uint64_t cr0 = read_cr0();
	uint64_t cr4 = read_cr4();

	cr0 &= ~(1ULL << 2); /* EM */
	cr0 |= (1ULL << 1); /* MP */
	cr4 |= (1ULL << 9) | (1ULL << 10); /* OSFXSR | OSXMMEXCPT */

	write_cr0(cr0);
	write_cr4(cr4);
	fninit();
}

void lyr_entry(void)
{
	__asm__ volatile("movq %%rsp, %0" : "=r"(_lyr_kstack_top));

	if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision))
		nointloop();

	async_io_init();
	async_test();

	log_info("entry", "Welcome to lyr-kernel " LYR_VERSION);
	log_info("entry", "UART %s", uart_init() == 0 ? "ok" : "not ok");
	cpu_enable_sse();

	/* framebuffer */
	LIMINE_REQUIRE(framebuffer_request);
	assert(framebuffer_request.response->framebuffer_count >= 1);
	assert(framebuffer_request.response->framebuffers[0] != NULL);
	struct limine_framebuffer *fb =
		framebuffer_request.response->framebuffers[0];

	lyrterm_apply_theme(&lyrterm_theme_gruvbox_dark);
	lyrterm_init(fb);

	/* etc requests */
	LIMINE_REQUIRE(bootloader_info_request);
	_lyr_bootloader_info = bootloader_info_request.response;

	LIMINE_REQUIRE(firmware_type_request);
	_lyr_firmware_type_info = firmware_type_request.response;

	LIMINE_REQUIRE(kernel_file_request);
	_lyr_file_info = kernel_file_request.response->executable_file;

	/* gdt and idt */
	gdt_init();
	gdt_tss_init(_lyr_kstack_top);
	log_info("entry", "GDT ok");
	idt_init();
	syscall_init();
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
	pmm_test();
	log_info("entry", "PMM ok");

	/* paging */
	LIMINE_REQUIRE(kernel_address_request);
	_lyr_kvirt = kernel_address_request.response->virtual_base;
	_lyr_kphys = kernel_address_request.response->physical_base;

	paging_init();
	assert(kernel_ptable != NULL);
	paging_test();
	log_info("entry", "Paging ok");

	/* heap / VMM */
	kheap_init();
	heap_test();
	log_info("entry", "Heap ok");

	_lyr_kernel_vas = vas_adopt(kernel_ptable);
	log_trace("entry", "switched to kernel page table");
	vas_switch(_lyr_kernel_vas);
	vmm_test(_lyr_kernel_vas);
	log_info("entry", "VMM ok");

	vfs_node_t *root = tmpfs_create_root(0755, 0, 0);
	log_info("entry", "tmpfs ok");
	assert(root);
	vfs_init(root);
	devfs_init();
	log_info("entry", "devfs ok");
	assert(console_init() == VFS_OK);
	log_info("entry", "console ok");
	assert(vfs_root() == root);
	log_info("entry", "VFS ok");
	vfs_tmpfs_test(_lyr_kernel_vas);
	assert(initrd_load_from_limine(module_request.response) == VFS_OK);

	assert(device_system_init() == VFS_OK);
	log_info("entry", "Device system ok");
	assert(time_init() == VFS_OK);
	assert(rtc_init() == VFS_OK);
	log_info("entry", "RTC/time ok");
	assert(block_system_init() == VFS_OK);
	log_info("entry", "Block layer ok");
	assert(net_init() == VFS_OK);
	log_info("entry", "Network core ok");
	assert(socket_init() == VFS_OK);
	log_info("entry", "Socket layer ok");

	/* ACPI */
	LIMINE_REQUIRE(rsdp_request);
	acpi_init(rsdp_request.response->address);
	acpi_dump_tables();
	log_info("entry", "ACPI ok");
#if _DEBUG
	bgrt_init(fb); /* fun thing to test */
#endif
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

	/* scheduler */
	sched_init();

	/* timer */
	pit_init(1000);

	/* Give the PIT a moment to deliver at least one tick. */
	uint64_t t0 = pit_get_ticks();
	for (uint64_t i = 0; i < 10000000ULL && pit_get_ticks() == t0; i++)
		__asm__ volatile("pause" ::: "memory");

	if (pit_get_ticks() == t0)
		log_err("entry", "PIT (timer) failed");
	else {
		log_info("entry", "PIT (timer) ok");
		sched_test();
		log_info("entry", "Scheduler ok");
	}

	ipc_init();
	ipc_test();
	log_info("entry", "IPC ok");

	assert(driver_manager_init() == VFS_OK);
	log_info("entry", "Driver manager ok");

#if _DEBUG_INIT
	init_smoke_test();
#endif

	/* we are done, now launch init proc */
	log_info("entry", "lyr-kernel " LYR_VERSION
					  " done initializing, launching init proc");

	assert(init_spawn("/early-init") == VFS_OK);
	sched_exit(); /* finished */
}
