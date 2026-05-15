#include <drv/driver.h>
#include <stdarg.h>
#include <debug/log.h>
#include <dev/block.h>
#include <dev/console.h>
#include <dev/device.h>
#include <fs/evdev.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <ipc/ipc.h>
#include <lib/elf.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/page.h>
#include <mm/paging.h>
#include <mm/pfndb.h>
#include <mm/pmm.h>
#include <net/net.h>
#include <mm/vmm.h>
#include <sched/sched.h>
#include <sync/spinlock.h>
#include <util/kprintf.h>
#include <dev/kbd.h>

#define MODULE_REGION_BASE 0xffffffffa0000000ULL
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#define STT_FUNC 2

typedef struct {
	const char *name;
	uint64_t value;
	bool text;
} kernel_symbol_t;

typedef struct module_symbol {
	char name[DRIVER_NAME_MAX + 1];
	uint64_t value;
	driver_t *driver;
	struct module_symbol *next;
} module_symbol_t;

typedef struct trace_symbol {
	const char *owner;
	const char *name;
	uint64_t value;
	struct trace_symbol *next;
} trace_symbol_t;

extern int npf_snprintf_(char *buffer, size_t bufsz, const char *format, ...);
extern int npf_vsnprintf(char *buffer, size_t bufsz, const char *format,
						 va_list vlist);

/* public kernel symbols */
static const kernel_symbol_t kernel_symbols[] = {
	{ "devfs_mkdir", (uint64_t)devfs_mkdir, true },
	{ "devfs_register_chr", (uint64_t)devfs_register_chr, true },
	{ "devfs_register_chr_poll", (uint64_t)devfs_register_chr_poll, true },
	{ "console_input_put", (uint64_t)console_input_put, true },
	{ "block_register", (uint64_t)block_register, true },
	{ "device_handler_register", (uint64_t)device_handler_register, true },
	{ "device_register", (uint64_t)device_register, true },
	{ "driver_log", (uint64_t)driver_log, true },
	{ "get_phys", (uint64_t)get_phys, true },
	{ "ipc_call", (uint64_t)ipc_call, true },
	{ "ipc_endpoint_register", (uint64_t)ipc_endpoint_register, true },
	{ "ipc_notify", (uint64_t)ipc_notify, true },
	{ "ipc_shm_create", (uint64_t)ipc_shm_create, true },
	{ "ipc_shm_open", (uint64_t)ipc_shm_open, true },
	{ "kprintf", (uint64_t)kprintf, true },
	{ "kernel_ptable", (uint64_t)&kernel_ptable, false },
	{ "_lyr_hhdm_offset", (uint64_t)&_lyr_hhdm_offset, false },
	{ "map_mmio", (uint64_t)map_mmio, true },
	{ "sched_map_kernel_mmio", (uint64_t)sched_map_kernel_mmio, true },
	{ "map_page_phys", (uint64_t)map_page_phys, true },
	{ "memcpy", (uint64_t)memcpy, true },
	{ "memset", (uint64_t)memset, true },
	{ "net_receive_frame", (uint64_t)net_receive_frame, true },
	{ "net_tcp_listen", (uint64_t)net_tcp_listen, true },
	{ "net_poll_all", (uint64_t)net_poll_all, true },
	{ "net_default_dev", (uint64_t)net_default_dev, true },
	{ "netdev_count", (uint64_t)netdev_count, true },
	{ "net_ipv4_format", (uint64_t)net_ipv4_format, true },
	{ "netdev_register", (uint64_t)netdev_register, true },
	{ "npf_snprintf_", (uint64_t)npf_snprintf_, true },
	{ "npf_vsnprintf", (uint64_t)npf_vsnprintf, true },
	{ "palloc_single", (uint64_t)palloc_single, true },
	{ "strlen", (uint64_t)strlen, true },
	{ "strcpy", (uint64_t)strcpy, true },
	{ "vfs_root_cred", (uint64_t)&vfs_root_cred, false },
	{ "vfs_close", (uint64_t)vfs_close, true },
	{ "vfs_open", (uint64_t)vfs_open, true },
	{ "vfs_read", (uint64_t)vfs_read, true },
	{ "vfs_write", (uint64_t)vfs_write, true },
	{ "kzalloc", (uint64_t)kzalloc, true },
	{ "krealloc", (uint64_t)krealloc, true },
	{ "kfree", (uint64_t)kfree, true },
	{ "driver_spawn_thread", (uint64_t)driver_spawn_thread, true },
	{ "evdev_bind_path", (uint64_t)evdev_bind_path, true },
	{ "evdev_create", (uint64_t)evdev_create, true },
	{ "evdev_flush", (uint64_t)evdev_flush, true },
	{ "evdev_init", (uint64_t)evdev_init, true },
	{ "evdev_push", (uint64_t)evdev_push, true },
	{ "evdev_read_bytes", (uint64_t)evdev_read_bytes, true },
	{ "evdev_read_record", (uint64_t)evdev_read_record, true },
	{ "vfs_node_release", (uint64_t)vfs_node_release, true },
	{ "vfs_readdir", (uint64_t)vfs_readdir, true },
	{ "vfs_resolve", (uint64_t)vfs_resolve, true },
	{ "vfs_stat", (uint64_t)vfs_stat, true },
	{ "memmove", (uint64_t)memmove, true },
	{ "strcmp", (uint64_t)strcmp, true },
	{ "strncmp", (uint64_t)strncmp, true },
	{ "strstr", (uint64_t)strstr, true },
	{ "net_default_dev", (uint64_t)net_default_dev, true },
	{ "net_ipv4_format", (uint64_t)net_ipv4_format, true },
	{ "netdev_count", (uint64_t)netdev_count, true },
	{ "kbd_submit_event", (uint64_t)kbd_submit_event, true },
};

static bool kernel_symbol_lookup(uint64_t addr, driver_symbol_info_t *out)
{
	const kernel_symbol_t *best = NULL;

	for (size_t i = 0; i < sizeof(kernel_symbols) / sizeof(kernel_symbols[0]);
		 i++) {
		uint64_t sym_addr = kernel_symbols[i].value;
		if (!kernel_symbols[i].text)
			continue;
		if (sym_addr > addr)
			continue;
		if (!best || sym_addr > best->value)
			best = &kernel_symbols[i];
	}

	if (!best)
		return false;

	out->owner = "kernel";
	out->name = best->name;
	out->address = best->value;
	return true;
}

static const char *boot_driver_paths[] = {
	"/sys/pci.sys",
	"/sys/nvme.sys",
	"/sys/e1000.sys",
	/*"/sys/websrv.sys",*/ "/sys/ps2.sys",
};

static spinlock_t driver_lock = SPINLOCK_INIT;
static spinlock_t module_alloc_lock = SPINLOCK_INIT;
static driver_t *loaded_drivers[16];
static size_t loaded_driver_count = 0;
static module_symbol_t *module_symbols = NULL;
static trace_symbol_t *trace_symbols = NULL;
static uint64_t next_module_base = MODULE_REGION_BASE;

static void copy_cstr(char *dst, size_t dst_len, const char *src)
{
	size_t i = 0;
	if (src) {
		for (; i + 1 < dst_len && src[i]; i++)
			dst[i] = src[i];
	}
	dst[i] = '\0';
}

static uint64_t align_up_u64(uint64_t value, uint64_t align)
{
	if (align <= 1)
		return value;
	return (value + align - 1) & ~(align - 1);
}

static void *module_alloc_section(uint64_t size, uint64_t align, void *ctx)
{
	(void)ctx;
	if (size == 0)
		return NULL;
	if (align < PAGE_SIZE)
		align = PAGE_SIZE;

	spinlock_acquire(&module_alloc_lock);
	uint64_t base = align_up_u64(next_module_base, align);
	uint64_t end = align_up_u64(base + size, PAGE_SIZE);
	for (uint64_t va = base; va < end; va += PAGE_SIZE) {
		page_t *page = palloc_page();
		if (!page) {
			spinlock_release(&module_alloc_lock);
			return NULL;
		}
		map_page(kernel_ptable, va, page, VMM_PRESENT | VMM_WRITABLE);
		page_unref(page);
	}
	next_module_base = end;
	spinlock_release(&module_alloc_lock);

	memset((void *)base, 0, (size_t)(end - base));
	return (void *)base;
}

static module_symbol_t *module_symbol_find_locked(const char *name)
{
	for (module_symbol_t *sym = module_symbols; sym; sym = sym->next) {
		if (strcmp(sym->name, name) == 0)
			return sym;
	}
	return NULL;
}

static const module_symbol_t *module_symbol_lookup_locked(uint64_t addr)
{
	const module_symbol_t *best = NULL;

	for (module_symbol_t *sym = module_symbols; sym; sym = sym->next) {
		if (sym->value > addr)
			continue;
		if (!best || sym->value > best->value)
			best = sym;
	}

	return best;
}

static const trace_symbol_t *trace_symbol_lookup_locked(uint64_t addr)
{
	const trace_symbol_t *best = NULL;

	for (trace_symbol_t *sym = trace_symbols; sym; sym = sym->next) {
		if (sym->value > addr)
			continue;
		if (!best || sym->value > best->value)
			best = sym;
	}

	return best;
}

static int driver_register_trace_symbols(driver_t *driver, elf_image_t *image)
{
	if (!driver || !image || !image->symtab || !image->strtab)
		return 0;

	for (size_t i = 0; i < image->sym_count; i++) {
		const elf64_sym_t *sym = &image->symtab[i];
		if (sym->st_shndx == ELF_SHN_UNDEF)
			continue;
		if (ELF64_ST_TYPE(sym->st_info) != STT_FUNC)
			continue;
		const char *name = elf_symbol_name(image, sym);
		if (!name || !*name)
			continue;

		trace_symbol_t *trace = kzalloc(sizeof(*trace));
		if (!trace)
			return -ENOMEM;
		trace->owner = driver->name;
		trace->name = name;
		trace->value = (uint64_t)image->sections[sym->st_shndx] + sym->st_value;

		spinlock_acquire(&driver_lock);
		trace->next = trace_symbols;
		trace_symbols = trace;
		spinlock_release(&driver_lock);
	}

	return 0;
}

static int module_symbol_resolve(const char *name, uint64_t *out, void *ctx)
{
	(void)ctx;
	if (!name || !out)
		return -EINVAL;

	for (size_t i = 0; i < sizeof(kernel_symbols) / sizeof(kernel_symbols[0]);
		 i++) {
		if (strcmp(kernel_symbols[i].name, name) == 0) {
			*out = kernel_symbols[i].value;
			return 0;
		}
	}

	spinlock_acquire(&driver_lock);
	module_symbol_t *sym = module_symbol_find_locked(name);
	if (sym) {
		*out = sym->value;
		spinlock_release(&driver_lock);
		return 0;
	}
	spinlock_release(&driver_lock);
	return -ENOENT;
}

bool driver_lookup_symbol(uint64_t addr, driver_symbol_info_t *out)
{
	if (!out)
		return false;

	driver_symbol_info_t best = { 0 };
	bool found = false;

	if (kernel_symbol_lookup(addr, &best))
		found = true;

	spinlock_acquire(&driver_lock);
	const trace_symbol_t *trace = trace_symbol_lookup_locked(addr);
	if (trace && (!found || trace->value > best.address)) {
		best.owner = trace->owner ? trace->owner : "driver";
		best.name = trace->name;
		best.address = trace->value;
		found = true;
	}
	const module_symbol_t *mod = module_symbol_lookup_locked(addr);
	if (mod && (!found || mod->value > best.address)) {
		best.owner =
			mod->driver && mod->driver->name[0] ? mod->driver->name : "driver";
		best.name = mod->name;
		best.address = mod->value;
		found = true;
	}
	spinlock_release(&driver_lock);

	if (!found)
		return false;

	*out = best;
	return true;
}

static driver_metadata_t *driver_metadata_from_elf(elf_image_t *image)
{
	uint64_t value = 0;
	if (elf_find_symbol_value(image, "lyr_driver_metadata", &value,
							  module_symbol_resolve, NULL) != 0)
		return NULL;
	return (driver_metadata_t *)value;
}

static int driver_metadata_imports_resolved(driver_t *driver)
{
	const driver_metadata_t *m = driver->metadata;
	for (size_t i = 0; i < m->import_count; i++) {
		uint64_t value = 0;
		if (module_symbol_resolve(m->imports[i], &value, NULL) != 0) {
			log_err("driver", "%s missing import %s", driver->name,
					m->imports[i]);
			return -ENOENT;
		}
	}
	return 0;
}

static int driver_register_exports(driver_t *driver, elf_image_t *image)
{
	const driver_metadata_t *m = driver->metadata;
	for (size_t i = 0; i < m->export_count; i++) {
		uint64_t value = 0;
		int r = elf_find_defined_symbol_value(image, m->exports[i], &value);
		if (r != 0) {
			log_err("driver", "%s export %s is not defined", driver->name,
					m->exports[i]);
			return r;
		}

		module_symbol_t *sym = kzalloc(sizeof(*sym));
		if (!sym)
			return -ENOMEM;
		copy_cstr(sym->name, sizeof(sym->name), m->exports[i]);
		sym->value = value;
		sym->driver = driver;

		spinlock_acquire(&driver_lock);
		if (module_symbol_find_locked(sym->name)) {
			spinlock_release(&driver_lock);
			kfree(sym);
			return -EEXIST;
		}
		sym->next = module_symbols;
		module_symbols = sym;
		spinlock_release(&driver_lock);
		log_trace("driver", "%s exports %s=0x%llx", driver->name, sym->name,
				  sym->value);
	}
	return 0;
}

static int driver_runtime_write_file(const char *path, const char *data)
{
	vfs_file_t *file = NULL;
	int r = vfs_open(path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0644,
					 &vfs_root_cred, &file);
	if (r != 0)
		return r;
	size_t done = 0;
	r = vfs_write(file, data, strlen(data), &done);
	vfs_close(file);
	return r == 0 && done == strlen(data) ? 0 : r;
}

void driver_log(driver_t *driver, const char *level, const char *message)
{
	char subsys[64];

	const char *name = (driver && driver->name[0]) ? driver->name : "unknown";

	npf_snprintf(subsys, sizeof(subsys), "driver/%s", name);

	if (!level)
		level = "info";

	const char *msg = message ? message : "";

	if (strcmp(level, "trace") == 0 || strcmp(level, "trce") == 0) {
		log_trace(subsys, "%s", msg);
	} else if (strcmp(level, "debug") == 0 || strcmp(level, "dbug") == 0) {
		log_debug(subsys, "%s", msg);
	} else if (strcmp(level, "warn") == 0) {
		log_warn(subsys, "%s", msg);
	} else if (strcmp(level, "err") == 0 || strcmp(level, "error") == 0) {
		log_err(subsys, "%s", msg);
	} else {
		log_info(subsys, "%s", msg);
	}
}

int driver_spawn_thread(driver_t *driver, const char *name,
						driver_thread_entry_t entry, void *arg)
{
	if (!driver || !driver->process || !entry)
		return -EINVAL;
	tcb_t *thread =
		sched_create_thread((pcb_t *)driver->process, name, entry, arg);
	return thread ? 0 : -ENOMEM;
}

static int driver_read_file(const char *path, uint8_t **out, size_t *out_size)
{
	vfs_file_t *file = NULL;
	int r = vfs_open(path, VFS_O_RDONLY, 0, &vfs_root_cred, &file);
	if (r != 0)
		return r;
	size_t size = (size_t)file->node->size;
	uint8_t *buf = kzalloc(size + 1);
	if (!buf) {
		vfs_close(file);
		return -ENOMEM;
	}
	size_t done = 0;
	r = vfs_read(file, buf, size, &done);
	vfs_close(file);
	if (r != 0) {
		kfree(buf);
		return r;
	}
	*out = buf;
	*out_size = done;
	return 0;
}

static int driver_load_module(const char *path)
{
	uint8_t *file = NULL;
	size_t file_size = 0;
	int r = driver_read_file(path, &file, &file_size);
	if (r != 0) {
		log_err("driver", "cannot read %s status=%d", path, r);
		return r;
	}

	elf_image_t image;
	r = elf_load_relocatable(&image, file, file_size, module_alloc_section,
							 NULL, module_symbol_resolve, NULL);
	if (r != 0) {
		kfree(file);
		kfree(image.sections);
		return r;
	}

	driver_metadata_t *metadata = driver_metadata_from_elf(&image);
	if (!metadata || metadata->magic != DRIVER_MAGIC ||
		metadata->abi_version != DRIVER_ABI_VERSION || !metadata->name ||
		!metadata->entry) {
		kfree(file);
		kfree(image.sections);
		return -EINVAL;
	}

	driver_t *driver = kzalloc(sizeof(*driver));
	if (!driver) {
		kfree(file);
		kfree(image.sections);
		return -ENOMEM;
	}
	copy_cstr(driver->name, sizeof(driver->name), metadata->name);
	copy_cstr(driver->image_path, sizeof(driver->image_path), path);
	driver->image = image.sections;
	driver->image_size = image.section_count;
	driver->metadata = metadata;

	r = driver_metadata_imports_resolved(driver);
	if (r != 0)
		return r;

	pcb_t *process = sched_process_create(driver->name, _lyr_kernel_vas);
	if (!process)
		return -ENOMEM;
	driver->pid = process->pid;
	driver->process = process;

	log_debug("driver", "loaded ELF %s as %s pid=%d", path, driver->name,
			  driver->pid);
	r = driver_register_exports(driver, &image);
	if (r != 0)
		return r;

	r = driver_register_trace_symbols(driver, &image);
	if (r != 0)
		return r;

	driver->status = metadata->entry(driver);
	if (driver->status != 0)
		return driver->status;

	spinlock_acquire(&driver_lock);
	if (loaded_driver_count <
		sizeof(loaded_drivers) / sizeof(loaded_drivers[0]))
		loaded_drivers[loaded_driver_count++] = driver;
	spinlock_release(&driver_lock);

	char pathbuf[96];
	char body[256];
	npf_snprintf(pathbuf, sizeof(pathbuf), "/run/drivers/%s", driver->name);
	npf_snprintf(body, sizeof(body),
				 "name=%s\npid=%d\nimage=%s\nexports=%zu\nimports=%zu\n",
				 driver->name, driver->pid, driver->image_path,
				 metadata->export_count, metadata->import_count);
	driver_runtime_write_file(pathbuf, body);
	return 0;
}

int driver_manager_init(void)
{
	int r = vfs_mkdir("/run", 0755, &vfs_root_cred);
	if (r != 0 && r != -EEXIST)
		return r;
	r = vfs_mkdir("/run/drivers", 0755, &vfs_root_cred);
	if (r != 0 && r != -EEXIST)
		return r;

	for (size_t i = 0;
		 i < sizeof(boot_driver_paths) / sizeof(boot_driver_paths[0]); i++) {
		r = driver_load_module(boot_driver_paths[i]);
		if (r != 0)
			log_err("driver", "load %s failed status=%d", boot_driver_paths[i],
					r);
	}
	log_debug("driver", "manager loaded %zu ELF driver(s)",
			  loaded_driver_count);
	return 0;
}
