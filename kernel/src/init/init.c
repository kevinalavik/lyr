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
#define INIT_STACK_SIZE (8 * 1024 * 1024ULL)
#define INIT_STACK_GUARD_SIZE PAGE_SIZE

int init_spawn(const char *path)
{
	if (!path)
		return -EINVAL;

	vas_t *vas = vas_create(NULL);
	if (!vas)
		return -ENOMEM;

	elf_user_image_t image;
	int r = elf_load_user_executable(vas, path, &image);
	if (r != 0) {
		log_err("init", "failed to load %s status=%s(%d)", path, errno_name(r),
				r);
		vas_destroy(vas);
		return r;
	}

	uint64_t stack_base =
		INIT_STACK_TOP - (INIT_STACK_SIZE + INIT_STACK_GUARD_SIZE);
	uint64_t stack_map_base = stack_base + INIT_STACK_GUARD_SIZE;
	uint64_t stack = vas_map_anon(vas, stack_map_base, INIT_STACK_SIZE,
								  VMM_PRESENT | VMM_WRITABLE | VMM_USER |
									  VMM_NX | VAD_FIXED);
	if (stack != stack_map_base) {
		log_err("init", "failed to map userspace stack for %s", path);
		vas_destroy(vas);
		return -ENOMEM;
	}

	const char *argv[] = { path, NULL };
	uint64_t user_rsp = 0;
	r = elf_build_initial_stack(vas, INIT_STACK_TOP, path, argv, 1, NULL, 0,
								&image, &user_rsp);
	if (r != 0) {
		log_err("init", "failed to build initial stack for %s status=%s(%d)",
				path, errno_name(r), r);
		vas_destroy(vas);
		return r;
	}

	pcb_t *process = sched_process_create("init", vas);
	if (!process) {
		vas_destroy(vas);
		return -ENOMEM;
	}

	tcb_t *thread =
		sched_create_user_thread(process, "init", image.entry, user_rsp);
	if (!thread)
		return -ENOMEM;

	log_debug("init", "spawned %s entry=0x%llx stack=0x%llx", path, image.entry,
			  user_rsp);
	return 0;
}
