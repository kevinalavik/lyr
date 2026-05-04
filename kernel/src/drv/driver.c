#include <drv/driver.h>
#include <debug/log.h>
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
#include <mm/vmm.h>
#include <sched/sched.h>
#include <sync/spinlock.h>
#include <util/kprintf.h>

#define MODULE_REGION_BASE 0xffffffffa0000000ULL

typedef struct {
	const char *name;
	uint64_t value;
} kernel_symbol_t;

typedef struct module_symbol {
	char name[DRIVER_NAME_MAX + 1];
	uint64_t value;
	driver_t *driver;
	struct module_symbol *next;
} module_symbol_t;

extern int npf_snprintf_(char *buffer, size_t bufsz, const char *format, ...);

/* public kernel symbols */
static const kernel_symbol_t kernel_symbols[] = {
	{ "devfs_mkdir", (uint64_t)devfs_mkdir },
	{ "devfs_register_chr", (uint64_t)devfs_register_chr },
	{ "driver_log", (uint64_t)driver_log },
	{ "ipc_call", (uint64_t)ipc_call },
	{ "ipc_endpoint_register", (uint64_t)ipc_endpoint_register },
	{ "ipc_notify", (uint64_t)ipc_notify },
	{ "ipc_shm_create", (uint64_t)ipc_shm_create },
	{ "ipc_shm_open", (uint64_t)ipc_shm_open },
	{ "kprintf", (uint64_t)kprintf },
	{ "memcpy", (uint64_t)memcpy },
	{ "memset", (uint64_t)memset },
	{ "npf_snprintf_", (uint64_t)npf_snprintf_ },
	{ "strlen", (uint64_t)strlen },
	{ "vfs_close", (uint64_t)vfs_close },
	{ "vfs_open", (uint64_t)vfs_open },
	{ "vfs_read", (uint64_t)vfs_read },
	{ "kzalloc", (uint64_t)kzalloc },
	{ "kfree", (uint64_t)kfree },
};

static const char *boot_driver_paths[] = {
	"/sys/pci.sys",
};

static spinlock_t driver_lock = SPINLOCK_INIT;
static spinlock_t module_alloc_lock = SPINLOCK_INIT;
static driver_t *loaded_drivers[16];
static size_t loaded_driver_count = 0;
static module_symbol_t *module_symbols = NULL;
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

static int module_symbol_resolve(const char *name, uint64_t *out, void *ctx)
{
	(void)ctx;
	if (!name || !out)
		return VFS_ERR_INVAL;

	for (size_t i = 0; i < sizeof(kernel_symbols) / sizeof(kernel_symbols[0]);
		 i++) {
		if (strcmp(kernel_symbols[i].name, name) == 0) {
			*out = kernel_symbols[i].value;
			return VFS_OK;
		}
	}

	spinlock_acquire(&driver_lock);
	module_symbol_t *sym = module_symbol_find_locked(name);
	if (sym) {
		*out = sym->value;
		spinlock_release(&driver_lock);
		return VFS_OK;
	}
	spinlock_release(&driver_lock);
	return VFS_ERR_NOENT;
}

static driver_metadata_t *driver_metadata_from_elf(elf_image_t *image)
{
	uint64_t value = 0;
	if (elf_find_symbol_value(image, "lyr_driver_metadata", &value,
							  module_symbol_resolve, NULL) != VFS_OK)
		return NULL;
	return (driver_metadata_t *)value;
}

static int driver_metadata_imports_resolved(driver_t *driver)
{
	const driver_metadata_t *m = driver->metadata;
	for (size_t i = 0; i < m->import_count; i++) {
		uint64_t value = 0;
		if (module_symbol_resolve(m->imports[i], &value, NULL) != VFS_OK) {
			log_err("driver", "%s missing import %s", driver->name,
					m->imports[i]);
			return VFS_ERR_NOENT;
		}
	}
	return VFS_OK;
}

static int driver_register_exports(driver_t *driver, elf_image_t *image)
{
	const driver_metadata_t *m = driver->metadata;
	for (size_t i = 0; i < m->export_count; i++) {
		uint64_t value = 0;
		int r = elf_find_defined_symbol_value(image, m->exports[i], &value);
		if (r != VFS_OK) {
			log_err("driver", "%s export %s is not defined", driver->name,
					m->exports[i]);
			return r;
		}

		module_symbol_t *sym = kzalloc(sizeof(*sym));
		if (!sym)
			return VFS_ERR_NOMEM;
		copy_cstr(sym->name, sizeof(sym->name), m->exports[i]);
		sym->value = value;
		sym->driver = driver;

		spinlock_acquire(&driver_lock);
		if (module_symbol_find_locked(sym->name)) {
			spinlock_release(&driver_lock);
			kfree(sym);
			return VFS_ERR_EXIST;
		}
		sym->next = module_symbols;
		module_symbols = sym;
		spinlock_release(&driver_lock);
		log_trace("driver", "%s exports %s=0x%llx", driver->name, sym->name,
				  sym->value);
	}
	return VFS_OK;
}

static int driver_runtime_write_file(const char *path, const char *data)
{
	vfs_file_t *file = NULL;
	int r = vfs_open(path, VFS_O_CREAT | VFS_O_WRONLY | VFS_O_TRUNC, 0644,
					 &vfs_root_cred, &file);
	if (r != VFS_OK)
		return r;
	size_t done = 0;
	r = vfs_write(file, data, strlen(data), &done);
	vfs_close(file);
	return r == VFS_OK && done == strlen(data) ? VFS_OK : r;
}

void driver_log(driver_t *driver, const char *level, const char *message)
{
	log_info(driver && driver->name[0] ? driver->name : "driver", "%s: %s",
			 level ? level : "info", message ? message : "");
}

static int driver_read_file(const char *path, uint8_t **out, size_t *out_size)
{
	vfs_file_t *file = NULL;
	int r = vfs_open(path, VFS_O_RDONLY, 0, &vfs_root_cred, &file);
	if (r != VFS_OK)
		return r;
	size_t size = (size_t)file->node->size;
	uint8_t *buf = kzalloc(size + 1);
	if (!buf) {
		vfs_close(file);
		return VFS_ERR_NOMEM;
	}
	size_t done = 0;
	r = vfs_read(file, buf, size, &done);
	vfs_close(file);
	if (r != VFS_OK) {
		kfree(buf);
		return r;
	}
	*out = buf;
	*out_size = done;
	return VFS_OK;
}

static int driver_load_module(const char *path)
{
	uint8_t *file = NULL;
	size_t file_size = 0;
	int r = driver_read_file(path, &file, &file_size);
	if (r != VFS_OK) {
		log_err("driver", "cannot read %s status=%d", path, r);
		return r;
	}

	elf_image_t image;
	r = elf_load_relocatable(&image, file, file_size, module_alloc_section,
							 NULL, module_symbol_resolve, NULL);
	if (r != VFS_OK) {
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
		return VFS_ERR_INVAL;
	}

	driver_t *driver = kzalloc(sizeof(*driver));
	if (!driver) {
		kfree(file);
		kfree(image.sections);
		return VFS_ERR_NOMEM;
	}
	copy_cstr(driver->name, sizeof(driver->name), metadata->name);
	copy_cstr(driver->image_path, sizeof(driver->image_path), path);
	driver->image = image.sections;
	driver->image_size = image.section_count;
	driver->metadata = metadata;

	r = driver_metadata_imports_resolved(driver);
	if (r != VFS_OK)
		return r;

	pcb_t *process = sched_process_create(driver->name, _lyr_kernel_vas);
	if (!process)
		return VFS_ERR_NOMEM;
	driver->pid = process->pid;

	log_debug("driver", "loaded ELF %s as %s pid=%d", path, driver->name,
			  driver->pid);
	r = driver_register_exports(driver, &image);
	if (r != VFS_OK)
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
	return VFS_OK;
}

int driver_manager_init(void)
{
	int r = vfs_mkdir("/run", 0755, &vfs_root_cred);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;
	r = vfs_mkdir("/run/drivers", 0755, &vfs_root_cred);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;

	for (size_t i = 0;
		 i < sizeof(boot_driver_paths) / sizeof(boot_driver_paths[0]); i++) {
		r = driver_load_module(boot_driver_paths[i]);
		if (r != VFS_OK)
			log_err("driver", "load %s failed status=%d", boot_driver_paths[i],
					r);
	}
	log_debug("driver", "manager loaded %zu ELF driver(s)",
			  loaded_driver_count);
	return VFS_OK;
}
