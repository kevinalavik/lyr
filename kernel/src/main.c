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
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/vmm.h>
#include <debug/test.h>
#include <sys/acpi.h>
#include <sys/acpi/bgrt.h>
#include <sys/acpi/madt.h>
#include <sys/apic.h>
#include <dev/pit.h>
#include <dev/device.h>
#include <sys/smp.h>
#include <sched/sched.h>
#include <fs/initrd.h>
#include <fs/vfs.h>
#include <drv/driver.h>
#include <fs/devfs.h>
#include <ipc/ipc.h>
#include <net/net.h>

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

static char fs_type_char(vfs_mode_t mode)
{
	if (VFS_S_ISDIR(mode))
		return 'd';
	if (VFS_S_ISCHR(mode))
		return 'c';
	if (VFS_S_ISREG(mode))
		return '-';
	return '?';
}

static void fs_mode_string(vfs_mode_t mode, char out[11])
{
	out[0] = fs_type_char(mode);
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

static int fs_join_path(const char *parent, const char *name, char *out,
						size_t out_len)
{
	size_t parent_len = strlen(parent);
	size_t name_len = strlen(name);
	int need_slash = !(parent_len == 1 && parent[0] == '/');
	size_t total = parent_len + (need_slash ? 1 : 0) + name_len;
	if (total + 1 > out_len)
		return VFS_ERR_NAMETOOLONG;

	memcpy(out, parent, parent_len);
	size_t pos = parent_len;
	if (need_slash)
		out[pos++] = '/';
	memcpy(out + pos, name, name_len);
	out[pos + name_len] = '\0';
	return VFS_OK;
}

__attribute__((unused)) static void fs_list_recursive(const char *path)
{
	vfs_stat_t st;
	int r = vfs_stat(path, &vfs_root_cred, &st);
	if (r != VFS_OK) {
		kprintf("? %s status=%d\n", path, r);
		return;
	}

	char mode[11];
	fs_mode_string(st.mode, mode);
	kprintf("%-10s %3u %5u:%-5u %10llu %04o %s\n", mode, st.nlink, st.uid,
			st.gid, st.size, st.mode & VFS_S_PERM, path);

	if (!VFS_S_ISDIR(st.mode))
		return;

	vfs_node_t *dir = NULL;
	r = vfs_resolve(path, &vfs_root_cred, &dir);
	if (r != VFS_OK) {
		kprintf("! cannot open dir %s status=%d\n", path, r);
		return;
	}

	for (size_t i = 0;; i++) {
		vfs_dirent_t ent;
		r = vfs_readdir(dir, i, &ent);
		if (r == VFS_ERR_NOENT)
			break;
		if (r != VFS_OK) {
			kprintf("! readdir %s[%zu] status=%d\n", path, i, r);
			break;
		}

		char child_path[256];
		r = fs_join_path(path, ent.name, child_path, sizeof(child_path));
		if (r != VFS_OK) {
			kprintf("! path too long under %s/%s\n", path, ent.name);
			continue;
		}
		fs_list_recursive(child_path);
	}

	vfs_node_release(dir);
	kprintf("\n");
}

#define CAT_FILE(path_)                                                        \
	do {                                                                       \
		vfs_file_t *file__ = NULL;                                             \
		int r__ = vfs_open((path_), VFS_O_RDONLY, 0, &vfs_root_cred, &file__); \
		if (r__ != VFS_OK) {                                                   \
			kprintf("cat: failed to open %s status=%d\n", (path_), r__);       \
			break;                                                             \
		}                                                                      \
                                                                               \
		size_t cap__ = file__->node->size;                                     \
		if (cap__ == 0 || VFS_S_ISCHR(file__->node->mode))                     \
			cap__ = 4096;                                                      \
                                                                               \
		char *buf__ = kzalloc(cap__ + 1);                                      \
		if (!buf__) {                                                          \
			kprintf("cat: failed to allocate buffer for %s\n", (path_));       \
			vfs_close(file__);                                                 \
			break;                                                             \
		}                                                                      \
                                                                               \
		size_t done__ = 0;                                                     \
		r__ = vfs_read(file__, buf__, cap__, &done__);                         \
		vfs_close(file__);                                                     \
		if (r__ != VFS_OK) {                                                   \
			kprintf("cat: failed to read %s status=%d\n", (path_), r__);       \
			kfree(buf__);                                                      \
			break;                                                             \
		}                                                                      \
                                                                               \
		buf__[done__] = '\0';                                                  \
		kprintf("%s", buf__);                                                  \
		kfree(buf__);                                                          \
	} while (0)

void test(void *)
{
#define ANSI_GRAY "\x1b[38;2;120;120;120m"
#define ANSI_RESET "\x1b[0m"
#define INIT_LOG(fmt, ...) \
	kprintf(ANSI_GRAY "init: " fmt ANSI_RESET, ##__VA_ARGS__)

	CAT_FILE("/etc/banner");
	kprintf("\n");

	INIT_LOG("Hello, World!\n");
	INIT_LOG("motd\n");
	CAT_FILE("/etc/motd");
	INIT_LOG("netdevs\n");
	CAT_FILE("/dev/net/devices");

#if _DEBUG
	INIT_LOG("filesystem tree\n");
	kprintf("%-10s %3s %11s %10s %4s %s\n", "mode", "lnk", "uid:gid", "size",
			"perm", "path");
	fs_list_recursive("/");
#endif

#if _DEBUG
	{
		vfs_node_t *dir = NULL;
		int r = vfs_resolve("/dev/pci", &vfs_root_cred, &dir);
		if (r != VFS_OK) {
			INIT_LOG("pci: failed to open /dev/pci status=%d\n", r);
			return;
		}

		for (size_t i = 0;; i++) {
			vfs_dirent_t ent;
			r = vfs_readdir(dir, i, &ent);
			if (r == VFS_ERR_NOENT)
				break;
			if (r != VFS_OK) {
				INIT_LOG("pci: readdir /dev/pci[%zu] status=%d\n", i, r);
				break;
			}

			if (!strcmp(ent.name, "devices"))
				continue;

			char path[128];
			r = fs_join_path("/dev/pci", ent.name, path, sizeof(path));
			if (r != VFS_OK)
				continue;

			vfs_stat_t st;
			r = vfs_stat(path, &vfs_root_cred, &st);
			if (r != VFS_OK || !VFS_S_ISDIR(st.mode))
				continue;

			char info_path[160];
			r = fs_join_path(path, "info", info_path, sizeof(info_path));
			if (r != VFS_OK)
				continue;

			INIT_LOG("%s:\n", info_path);
			CAT_FILE(info_path);
			kprintf("\n");
		}

		vfs_node_release(dir);
	}
#endif

	{
		uint32_t target = net_ipv4(8, 8, 8, 8);
		const uint16_t ident = 0x4c59;
		const uint16_t count = 16;
		uint16_t sent = 0;
		uint16_t received = 0;
		uint64_t min_ms = (uint64_t)-1;
		uint64_t max_ms = 0;
		uint64_t total_ms = 0;
		char target_ip[24];

		net_ipv4_format(target, target_ip, sizeof(target_ip));
		INIT_LOG("PING %s: 40 data bytes\n", target_ip);
		for (uint16_t seq = 1; seq <= count; seq++) {
			net_ping_result_t reply;
			sent++;
			int r = net_ping_echo(target, ident, seq, 1500, &reply);
			if (r == VFS_OK) {
				received++;
				if (reply.time_ms < min_ms)
					min_ms = reply.time_ms;
				if (reply.time_ms > max_ms)
					max_ms = reply.time_ms;
				total_ms += reply.time_ms;
				char reply_ip[24];
				net_ipv4_format(reply.src_ip, reply_ip, sizeof(reply_ip));
				INIT_LOG("%u bytes from %s: icmp_seq=%u ttl=%u time=%llums\n",
						 reply.bytes, reply_ip, reply.seq, reply.ttl,
						 (unsigned long long)reply.time_ms);
			} else {
				INIT_LOG("Request timeout for icmp_seq %u (status=%d)\n", seq,
						 r);
			}
		}

		uint16_t lost = sent - received;
		uint16_t loss = sent ? (uint16_t)((lost * 100) / sent) : 0;
		INIT_LOG("--- %s ping statistics ---\n", target_ip);
		INIT_LOG("%u packets transmitted, %u received, %u%% packet loss\n",
				 sent, received, loss);
		if (received) {
			INIT_LOG("round-trip min/avg/max = %llu/%llu/%llums\n",
					 (unsigned long long)min_ms,
					 (unsigned long long)(total_ms / received),
					 (unsigned long long)max_ms);
		}
	}

	INIT_LOG("no shell, exiting.\n");
	sched_thread_exit(0);
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

	/* framebuffer */
	LIMINE_REQUIRE(framebuffer_request);
	assert(framebuffer_request.response->framebuffer_count >= 1);
	assert(framebuffer_request.response->framebuffers[0] != NULL);
	struct limine_framebuffer *fb =
		framebuffer_request.response->framebuffers[0];

	lyrterm_apply_theme(&lyrterm_theme_dracula);
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
	assert(root);
	vfs_init(root);
	devfs_init();
	assert(vfs_root() == root);
	log_info("entry", "VFS ok");
	vfs_tmpfs_test(_lyr_kernel_vas);
	assert(initrd_load_from_limine(module_request.response) == VFS_OK);

	assert(device_system_init() == VFS_OK);
	log_info("entry", "Device system ok");
	assert(net_init() == VFS_OK);
	log_info("entry", "Network core ok");

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

	/* we are done, now launch init proc */
	log_info("entry", "lyr-kernel " LYR_VERSION
					  " done initializing, launching init proc");

	pcb_t *p = sched_process_create("init", _lyr_kernel_vas);
	assert(p);
	sched_create_thread(p, "init", test, NULL);
	sched_exit(); /* finished */
}
