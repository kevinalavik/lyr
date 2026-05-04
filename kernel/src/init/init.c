#include <init/init.h>
#include <debug/log.h>
#include <fs/vfs.h>
#include <lib/elf.h>
#include <mm/heap.h>
#include <mm/page.h>
#include <mm/paging.h>
#include <mm/vmm.h>
#include <sched/sched.h>

#define INIT_STACK_TOP 0x00007ffffff000ULL
#define INIT_STACK_SIZE (16 * PAGE_SIZE)

int init_spawn(const char *path)
{
	if (!path)
		return VFS_ERR_INVAL;

	vas_t *vas = vas_create(NULL);
	if (!vas)
		return VFS_ERR_NOMEM;

	uint64_t entry = 0;
	int r = elf_load_user_executable(vas, path, &entry);
	if (r != VFS_OK) {
		log_err("init", "failed to load %s status=%s(%d)", path,
				vfs_err_name(r), r);
		vas_destroy(vas);
		return r;
	}

	uint64_t stack_base = INIT_STACK_TOP - INIT_STACK_SIZE;
	uint64_t stack = vas_map_anon(vas, stack_base, INIT_STACK_SIZE,
								  VMM_PRESENT | VMM_WRITABLE | VMM_USER |
									  VMM_NX | VAD_FIXED);
	if (stack != stack_base) {
		log_err("init", "failed to map userspace stack for %s", path);
		vas_destroy(vas);
		return VFS_ERR_NOMEM;
	}

	pcb_t *process = sched_process_create("init", vas);
	if (!process) {
		vas_destroy(vas);
		return VFS_ERR_NOMEM;
	}

	tcb_t *thread =
		sched_create_user_thread(process, "init", entry, INIT_STACK_TOP - 16);
	if (!thread)
		return VFS_ERR_NOMEM;

	log_debug("init", "spawned %s entry=0x%llx stack=0x%llx", path, entry,
			  INIT_STACK_TOP);
	return VFS_OK;
}
