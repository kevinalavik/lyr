#include <fs/initrd.h>
#include <fs/cpio.h>
#include <debug/log.h>
#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <lib/string.h>

static int _ensure_tmpfs_root(void)
{
	if (vfs_root()) {
		log_debug("initrd", "using existing VFS root for initrd extraction");
		return 0;
	}

	vfs_node_t *root = tmpfs_create_root(0755, 0, 0);
	if (!root)
		return -ENOMEM;
	vfs_init(root);
	log_info("initrd", "created tmpfs VFS root");
	return 0;
}

static int _looks_like_initrd(struct limine_file *file)
{
	if (!file)
		return 0;
	if (file->string && strcmp(file->string, "initrd") == 0)
		return 1;
	if (file->path) {
		size_t len = strlen(file->path);
		const char *suffix = "initrd.cpio";
		size_t suffix_len = strlen(suffix);
		if (len >= suffix_len &&
			memcmp(file->path + len - suffix_len, suffix, suffix_len) == 0)
			return 1;
	}
	return 0;
}

int initrd_load_from_limine(struct limine_module_response *modules)
{
	if (!modules || modules->module_count == 0 || !modules->modules) {
		log_warn("initrd", "no Limine modules provided");
		return -ENOENT;
	}

	struct limine_file *chosen = NULL;
	for (uint64_t i = 0; i < modules->module_count; i++) {
		struct limine_file *file = modules->modules[i];
		log_debug("initrd", "module[%llu] path=%s string=%s size=%llu addr=%p",
				  i, file && file->path ? file->path : "(null)",
				  file && file->string ? file->string : "(null)",
				  file ? file->size : 0, file ? file->address : NULL);
		if (_looks_like_initrd(file)) {
			chosen = file;
			break;
		}
	}
	if (!chosen)
		chosen = modules->modules[0];
	if (!chosen || !chosen->address || chosen->size == 0)
		return -EINVAL;

	int r = _ensure_tmpfs_root();
	if (r != 0)
		return r;

	size_t entries = 0;
	r = cpio_newc_extract(chosen->address, chosen->size, &entries);
	if (r != 0) {
		log_err("initrd", "failed to extract %s status=%d",
				chosen->path ? chosen->path : "(module)", r);
		return r;
	}
	if (entries == 0) {
		log_err("initrd", "%s extracted zero entries; check initrd build root",
				chosen->path ? chosen->path : "(module)");
		return -ENOENT;
	}

	log_info("initrd", "loaded %zu entries from %s", entries,
			 chosen->path ? chosen->path : "(module)");
	return 0;
}
