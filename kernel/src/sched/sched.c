#include <sched/sched.h>
#include <cpu/gdt.h>
#include <cpu/instr.h>
#include <debug/assert.h>
#include <debug/log.h>
#include <debug/panic.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/page.h>
#include <mm/pmm.h>
#include <sys/apic.h>

#define KSTACK_REGION_BASE 0xffffffff90000000ULL
#define KSTACK_GUARD_SIZE PAGE_SIZE
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS 0x1B
#define USER_DS 0x23
#define RFLAGS_IF (1ULL << 9)
#define RFLAGS_RESERVED (1ULL << 1)

#define SCHED_SIGTERM 15
#define SCHED_SIGKILL 9

extern void sched_iret_to_frame(interrupt_frame_t *frame)
	__attribute__((noreturn));
extern void sched_iret_to_user(uint64_t rip, uint64_t rsp)
	__attribute__((noreturn));
extern void sched_switch_to_user(ptable_t *pml4, uint64_t rip, uint64_t rsp)
	__attribute__((noreturn));

static spinlock_t sched_lock = SPINLOCK_INIT;
static spinlock_t kstack_lock = SPINLOCK_INIT;
static atomic_bool sched_ready = false;
static atomic_uint reap_inflight = 0;
static uint64_t next_kstack_base = KSTACK_REGION_BASE;
static pcb_t *process_list = NULL;

typedef struct id_node {
	int id;
	struct id_node *next;
} id_node_t;

typedef struct {
	spinlock_t lock;
	int next;
	id_node_t *free;
} id_pool_t;

static id_pool_t pid_pool = { .lock = SPINLOCK_INIT, .next = 1, .free = NULL };
static id_pool_t tid_pool = { .lock = SPINLOCK_INIT, .next = 1, .free = NULL };

static uint64_t irq_save(void)
{
	uint64_t flags = read_rflags();
	cli();
	return flags;
}

static void irq_restore(uint64_t flags)
{
	if (flags & RFLAGS_IF)
		sti();
}

static void name_set(char *dst, size_t dst_len, const char *src)
{
	size_t i = 0;
	if (src) {
		for (; i < dst_len - 1 && src[i]; i++)
			dst[i] = src[i];
	}
	dst[i] = '\0';
}


static void process_refresh_cred(pcb_t *process)
{
	if (!process)
		return;

	process->cred.uid = process->euid;
	process->cred.gid = process->egid;
	process->cred.group_count = 0;
	process->cred.umask = 0022;
}

const vfs_cred_t *sched_process_cred(const pcb_t *process)
{
	return process ? &process->cred : &vfs_root_cred;
}

void sched_process_set_parent(pcb_t *process, pid_t ppid)
{
	if (!process)
		return;
	spinlock_acquire(&process->lock);
	process->ppid = ppid;
	spinlock_release(&process->lock);
}

void sched_process_set_name(pcb_t *process, const char *name)
{
	if (!process)
		return;

	spinlock_acquire(&process->lock);
	name_set(process->name, sizeof(process->name), name ? name : "process");
	spinlock_release(&process->lock);
}

void sched_thread_set_name(tcb_t *thread, const char *name)
{
	if (!thread)
		return;

	name_set(thread->name, sizeof(thread->name), name ? name : "thread");
}

void sched_process_copy_ids(pcb_t *dst, const pcb_t *src)
{
	if (!dst || !src)
		return;

	spinlock_acquire(&dst->lock);
	dst->ppid = src->pid;
	dst->ruid = src->ruid;
	dst->rgid = src->rgid;
	dst->suid = src->suid;
	dst->sgid = src->sgid;
	dst->euid = src->euid;
	dst->egid = src->egid;
	dst->cred = src->cred;
	spinlock_release(&dst->lock);
}

int sched_process_setuid(pcb_t *process, vfs_uid_t uid)
{
	if (!process)
		return -EINVAL;

	spinlock_acquire(&process->lock);
	if (process->euid != 0 && uid != process->ruid && uid != process->euid &&
		uid != process->suid) {
		spinlock_release(&process->lock);
		return -EPERM;
	}

	if (process->euid == 0) {
		process->ruid = uid;
		process->suid = uid;
	}
	process->euid = uid;
	process_refresh_cred(process);
	spinlock_release(&process->lock);
	return 0;
}

int sched_process_setgid(pcb_t *process, vfs_gid_t gid)
{
	if (!process)
		return -EINVAL;

	spinlock_acquire(&process->lock);
	if (process->euid != 0 && gid != process->rgid && gid != process->egid &&
		gid != process->sgid) {
		spinlock_release(&process->lock);
		return -EPERM;
	}

	if (process->euid == 0) {
		process->rgid = gid;
		process->sgid = gid;
	}
	process->egid = gid;
	process_refresh_cred(process);
	spinlock_release(&process->lock);
	return 0;
}

int sched_process_seteuid(pcb_t *process, vfs_uid_t uid)
{
	if (!process)
		return -EINVAL;

	spinlock_acquire(&process->lock);
	if (process->euid != 0 && uid != process->ruid && uid != process->suid) {
		spinlock_release(&process->lock);
		return -EPERM;
	}

	process->euid = uid;
	process_refresh_cred(process);
	spinlock_release(&process->lock);
	return 0;
}

int sched_process_setegid(pcb_t *process, vfs_gid_t gid)
{
	if (!process)
		return -EINVAL;

	spinlock_acquire(&process->lock);
	if (process->euid != 0 && gid != process->rgid && gid != process->sgid) {
		spinlock_release(&process->lock);
		return -EPERM;
	}

	process->egid = gid;
	process_refresh_cred(process);
	spinlock_release(&process->lock);
	return 0;
}

static void runq_push_locked(cpu_local_t *cpu, tcb_t *thread)
{
	thread->runq_next = NULL;
	thread->cpu = cpu;

	if (!cpu->runq_tail) {
		cpu->runq_head = thread;
		cpu->runq_tail = thread;
	} else {
		cpu->runq_tail->runq_next = thread;
		cpu->runq_tail = thread;
	}

	log_trace("sched", "cpu%u enqueue tid=%d pid=%d load=%u", cpu->cpu_index,
			  thread->tid, thread->process ? thread->process->pid : -1,
			  atomic_load(&cpu->sched_load));
}

static tcb_t *runq_pop_locked(cpu_local_t *cpu)
{
	tcb_t *thread = cpu->runq_head;
	if (!thread)
		return NULL;

	cpu->runq_head = thread->runq_next;
	if (!cpu->runq_head)
		cpu->runq_tail = NULL;
	thread->runq_next = NULL;
	return thread;
}

static cpu_local_t *least_loaded_cpu(void)
{
	cpu_local_t *best = &cpu_locals[0];
	unsigned best_load =
		atomic_load_explicit(&best->sched_load, memory_order_acquire);

	for (uint32_t i = 1; i < cpu_count; i++) {
		cpu_local_t *cpu = &cpu_locals[i];
		if (!atomic_load_explicit(&cpu->sched_ready, memory_order_acquire))
			continue;

		unsigned load =
			atomic_load_explicit(&cpu->sched_load, memory_order_acquire);
		if (load < best_load) {
			best = cpu;
			best_load = load;
		}
	}

	return best;
}

static uint64_t kstack_alloc(uint64_t size)
{
	spinlock_acquire(&kstack_lock);
	uint64_t base = next_kstack_base + KSTACK_GUARD_SIZE;
	uint64_t top = base + size;

	for (uint64_t va = base; va < top; va += PAGE_SIZE) {
		page_t *page = palloc_page();
		if (!page)
			kpanic(NULL, "sched: out of memory allocating kernel stack");
		map_page(kernel_ptable, va, page, VMM_PRESENT | VMM_WRITABLE | VMM_NX);
		page_unref(page);
	}

	next_kstack_base = top;
	spinlock_release(&kstack_lock);
	return top;
}

static int id_alloc(id_pool_t *pool)
{
	spinlock_acquire(&pool->lock);
	id_node_t *node = pool->free;
	if (node) {
		pool->free = node->next;
		int id = node->id;
		spinlock_release(&pool->lock);
		kfree(node);
		return id;
	}

	int id = pool->next++;
	spinlock_release(&pool->lock);
	return id;
}

static void id_free(id_pool_t *pool, int id)
{
	if (id <= 0)
		return;

	id_node_t *node = kzalloc(sizeof(id_node_t));
	if (!node)
		kpanic(NULL, "sched: failed to allocate free-id node");
	node->id = id;

	spinlock_acquire(&pool->lock);
	id_node_t **cur = &pool->free;
	while (*cur && (*cur)->id < id)
		cur = &(*cur)->next;

	if (*cur && (*cur)->id == id)
		kpanic(NULL, "sched: double-free of id %d", id);

	node->next = *cur;
	*cur = node;
	spinlock_release(&pool->lock);
}

static void kstack_free(uint64_t base, uint64_t top)
{
	if (!base || !top || top <= base)
		return;

	spinlock_acquire(&kstack_lock);
	for (uint64_t va = base; va < top; va += PAGE_SIZE)
		unmap_page(kernel_ptable, va);

	if (top == next_kstack_base)
		next_kstack_base = base - KSTACK_GUARD_SIZE;
	spinlock_release(&kstack_lock);
}

static interrupt_frame_t *thread_frame(tcb_t *thread)
{
	return (interrupt_frame_t *)thread->rsp;
}

static void process_unlink_locked(pcb_t *process)
{
	pcb_t **cur = &process_list;
	while (*cur) {
		if (*cur == process) {
			*cur = process->next;
			process->next = NULL;
			return;
		}
		cur = &(*cur)->next;
	}
}

static int waitpid_match(pid_t wanted, const pcb_t *child)
{
	if (!child)
		return 0;

	if (wanted == -1)
		return 1;

	return wanted > 0 && child->pid == wanted;
}

typedef enum process_exit_action {
	PROCESS_EXIT_NONE,
	PROCESS_EXIT_ZOMBIE,
	PROCESS_EXIT_REAP,
} process_exit_action_t;

static bool process_add_thread(pcb_t *process, tcb_t *thread)
{
	bool added = false;

	spinlock_acquire(&process->lock);
	if (!atomic_load_explicit(&process->dying, memory_order_acquire)) {
		thread->process_next = process->threads;
		process->threads = thread;
		atomic_fetch_add_explicit(&process->thread_count, 1,
								  memory_order_relaxed);
		added = true;
	}
	spinlock_release(&process->lock);

	return added;
}

static process_exit_action_t process_remove_thread(tcb_t *thread, int status)
{
	pcb_t *process = thread->process;
	if (!process)
		return PROCESS_EXIT_NONE;

	bool found = false;
	process_exit_action_t action = PROCESS_EXIT_NONE;

	spinlock_acquire(&process->lock);
	tcb_t **cur = &process->threads;
	while (*cur) {
		if (*cur == thread) {
			*cur = thread->process_next;
			thread->process_next = NULL;
			found = true;
			break;
		}
		cur = &(*cur)->process_next;
	}

	if (found) {
		unsigned old = atomic_fetch_sub_explicit(&process->thread_count, 1,
												 memory_order_acq_rel);
		unsigned left = old ? old - 1 : 0;
		if (left == 0 && process->pid != 0) {
			process->exit_status = status;
			atomic_store_explicit(&process->dying, true, memory_order_release);

			/*
			 * Only processes with a waitable parent stay in the process list as
			 * zombies. Kernel/internal processes are created with ppid == 0 and
			 * have nobody that can collect them via waitpid(), so they must be
			 * reaped by the scheduler as before.
			 */
			if (process->ppid > 0) {
				process->zombie = true;
				action = PROCESS_EXIT_ZOMBIE;
			} else {
				action = PROCESS_EXIT_REAP;
			}
		}
	}
	spinlock_release(&process->lock);

	return action;
}


static void process_reparent_children(pid_t old_parent, pid_t new_parent)
{
	if (old_parent <= 0)
		return;

	uint64_t flags = irq_save();
	spinlock_acquire(&sched_lock);
	for (pcb_t *child = process_list; child; child = child->next) {
		if (child->ppid == old_parent) {
			child->ppid = new_parent;
			log_trace("sched", "reparented pid=%d from ppid=%d to ppid=%d",
					  child->pid, old_parent, new_parent);
		}
	}
	spinlock_release(&sched_lock);
	irq_restore(flags);
}

static void process_note_zombie(pcb_t *process)
{
	if (!process || process->pid == 0)
		return;

	log_trace("sched", "process pid=%d (%s) became zombie", process->pid,
			  process->name);
}

static int process_signal_allowed(const pcb_t *sender, const pcb_t *target)
{
	if (!sender || !target)
		return 0;

	if (sender->euid == 0)
		return 1;

	return sender->euid == target->ruid || sender->euid == target->euid ||
		   sender->euid == target->suid;
}

static int process_is_kernel_owned(const pcb_t *process)
{
	return !process || process->pid <= 0 || process->vas == _lyr_kernel_vas ||
		   !process->owns_vas;
}

static int process_kill_status(int signal)
{
	return 128 + signal;
}

static void thread_finish_locked(cpu_local_t *cpu, tcb_t *thread,
							   interrupt_frame_t *frame, int status,
							   const char *how);

static void switch_to_thread(cpu_local_t *cpu, tcb_t *next)
{
	next->state = TCB_RUNNING;
	cpu->current_thread = next;
	if (next->kstack_top)
		gdt_set_kernel_stack(next->kstack_top);
	write_fs_base(next->fs_base);

	if (next->process && next->process->vas &&
		next->process->pml4 != (ptable_t *)read_cr3())
		vas_switch(next->process->vas);
}

static interrupt_frame_t *schedule_locked(cpu_local_t *cpu,
										  interrupt_frame_t *frame,
										  bool enqueue_current)
{
	tcb_t *prev = cpu->current_thread;

	if (prev && prev->state == TCB_RUNNING)
		prev->rsp = (uint64_t)frame;

	if (prev && !prev->is_idle && prev->state == TCB_RUNNING &&
		prev->process &&
		atomic_load_explicit(&prev->process->dying, memory_order_acquire))
		thread_finish_locked(cpu, prev, frame, prev->process->exit_status,
						   " via signal");

	if (enqueue_current && prev && !prev->is_idle &&
		prev->state == TCB_RUNNING) {
		prev->state = TCB_READY;
		runq_push_locked(cpu, prev);
	}

	tcb_t *next = NULL;
	while ((next = runq_pop_locked(cpu)) != NULL) {
		if (next->state != TCB_READY)
			continue;

		if (next->process &&
			atomic_load_explicit(&next->process->dying, memory_order_acquire)) {
			thread_finish_locked(cpu, next, NULL, next->process->exit_status,
							   " via signal");
			next = NULL;
			continue;
		}

		break;
	}

	if (!next)
		next = cpu->idle_thread;

	if (!next) {
		if (prev && prev->state == TCB_READY)
			prev->state = TCB_RUNNING;
		return frame;
	}

	if (next == prev && next->state == TCB_RUNNING)
		return frame;

	switch_to_thread(cpu, next);
	log_trace("sched", "cpu%u switch tid=%d -> tid=%d", cpu->cpu_index,
			  prev ? prev->tid : -1, next->tid);
	return thread_frame(next);
}

static void frame_init_kernel(tcb_t *thread, void (*rip)(tcb_t *))
{
	interrupt_frame_t *frame =
		(interrupt_frame_t *)(thread->kstack_top - sizeof(interrupt_frame_t));
	memset(frame, 0, sizeof(*frame));

	frame->rdi = (uint64_t)thread;
	frame->rip = (uint64_t)rip;
	frame->cs = KERNEL_CS;
	frame->rflags = RFLAGS_RESERVED | RFLAGS_IF;
	frame->rsp = thread->kstack_top;
	frame->ss = KERNEL_DS;
	frame->ds = KERNEL_DS;
	frame->es = KERNEL_DS;

	thread->rsp = (uint64_t)frame;
	thread->mode = TCB_MODE_KERNEL;
}

static void frame_init_user(tcb_t *thread, uint64_t rip, uint64_t user_rsp)
{
	interrupt_frame_t *frame =
		(interrupt_frame_t *)(thread->kstack_top - sizeof(interrupt_frame_t));
	memset(frame, 0, sizeof(*frame));

	frame->rip = rip;
	frame->cs = USER_CS;
	frame->rflags = RFLAGS_RESERVED | RFLAGS_IF;
	frame->rsp = user_rsp;
	frame->ss = USER_DS;
	frame->ds = USER_DS;
	frame->es = USER_DS;

	thread->user_entry_rsp = user_rsp;
	thread->rsp = (uint64_t)frame;
	thread->mode = TCB_MODE_USER;
}

void process_setup_fds(pcb_t *process)
{
	int r = vfs_open("/dev/stdin", VFS_O_RDONLY, 0, sched_process_cred(process),
					 &process->files[0]);
	if (r != 0)
		r = vfs_open("/dev/null", VFS_O_RDONLY, 0, sched_process_cred(process),
					 &process->files[0]);
	if (r != 0) {
		log_err("sched",
				"failed to open stdin for process %s status=%s(%d)",
				process->name, errno_name(r), r);
		atomic_store_explicit(&process->dying, true, memory_order_release);
		return;
	}

	r = vfs_open("/dev/stdout", VFS_O_WRONLY, 0, sched_process_cred(process),
				 &process->files[1]);
	if (r != 0) {
		log_err(
			"sched",
			"failed to open stdout for process %s status=%s(%d)",
			process->name, errno_name(r), r);
		vfs_close(process->files[0]);
		process->files[0] = NULL;
		atomic_store_explicit(&process->dying, true, memory_order_release);
		return;
	}

	r = vfs_open("/dev/stderr", VFS_O_WRONLY, 0, sched_process_cred(process),
				 &process->files[2]);
	if (r != 0) {
		log_err(
			"sched",
			"failed to open stderr for process %s status=%s(%d)",
			process->name, errno_name(r), r);
		vfs_close(process->files[0]);
		vfs_close(process->files[1]);
		process->files[0] = NULL;
		process->files[1] = NULL;
		atomic_store_explicit(&process->dying, true, memory_order_release);
		return;
	}

	log_debug("sched", "setup fd table for PID %d", process->pid);
	for (int i = 0; i < SCHED_FILE_MAX; i++) {
		vfs_file_t *f = process->files[i];
		if (!f)
			break;

		log_debug("sched", "fd[%d] -> %p", i, f);
	}
}

static pcb_t *process_alloc_id(const char *name, vas_t *vas, pid_t pid)
{
	pcb_t *process = kzalloc(sizeof(pcb_t));
	if (!process)
		return NULL;

	process->pid = pid;
	process->ppid = 0;
	name_set(process->name, sizeof(process->name), name ? name : "process");
	process->vas = vas ? vas : vas_create(NULL);
	if (!process->vas) {
		kfree(process);
		return NULL;
	}

	spinlock_init(&process->lock);
	atomic_init(&process->thread_count, 0);
	atomic_init(&process->dying, false);
	process->zombie = false;
	process->exit_status = 0;
	process->ruid = 0;
	process->rgid = 0;
	process->suid = 0;
	process->sgid = 0;
	process->euid = 0;
	process->egid = 0;
	memcpy(process->cwd, "/", 2);
	process_refresh_cred(process);

	process_setup_fds(process);

	process->pml4 = process->vas->pml4;
	process->owns_vas = process->vas != _lyr_kernel_vas;
	return process;
}

static pcb_t *process_alloc(const char *name, vas_t *vas)
{
	pid_t pid = (pid_t)id_alloc(&pid_pool);
	return process_alloc_id(name, vas, pid);
}

static void process_link_locked(pcb_t *process)
{
	process->next = process_list;
	process_list = process;
}

static void process_destroy(pcb_t *process)
{
	if (!process || process->pid == 0)
		return;

	uint64_t flags = irq_save();
	spinlock_acquire(&sched_lock);
	process_unlink_locked(process);
	spinlock_release(&sched_lock);
	irq_restore(flags);

	pid_t pid = process->pid;
	char name[sizeof(process->name)];
	name_set(name, sizeof(name), process->name);

	if (process->owns_vas && process->vas)
		vas_destroy(process->vas);

	for (size_t i = 0; i < SCHED_FILE_MAX; i++) {
		if (!process->files[i])
			continue;
		vfs_close(process->files[i]);
		process->files[i] = NULL;
	}

	id_free(&pid_pool, pid);
	kfree(process);
	log_debug("sched", "destroyed process pid=%d name=%s", pid, name);
}

static void thread_queue_reap(cpu_local_t *cpu, tcb_t *thread)
{
	spinlock_acquire(&cpu->reap_lock);
	thread->reap_next = cpu->reap_head;
	cpu->reap_head = thread;
	spinlock_release(&cpu->reap_lock);
}

static void thread_reap(tcb_t *thread)
{
	pcb_t *process = thread->process;
	bool reap_process = thread->reap_process;
	tid_t tid = thread->tid;
	pid_t pid = process ? process->pid : -1;

	log_trace("sched", "reaping tid=%d pid=%d status=%d", tid, pid,
			  thread->exit_status);
	kstack_free(thread->kstack_base, thread->kstack_top);
	if (tid > 0)
		id_free(&tid_pool, tid);
	kfree(thread);

	if (reap_process)
		process_destroy(process);
}

static void sched_reap_current_cpu(void)
{
	cpu_local_t *cpu = get_cpu_local();
	if (!cpu)
		return;

	tcb_t *current = cpu->current_thread;
	for (;;) {
		tcb_t **cur;
		tcb_t *thread = NULL;

		spinlock_acquire(&cpu->reap_lock);
		cur = &cpu->reap_head;
		while (*cur) {
			if (*cur != current) {
				thread = *cur;
				*cur = thread->reap_next;
				thread->reap_next = NULL;
				break;
			}
			cur = &(*cur)->reap_next;
		}
		spinlock_release(&cpu->reap_lock);

		if (!thread)
			break;
		atomic_fetch_add_explicit(&reap_inflight, 1, memory_order_acq_rel);
		thread_reap(thread);
		atomic_fetch_sub_explicit(&reap_inflight, 1, memory_order_acq_rel);
	}
}

static void thread_finish_locked(cpu_local_t *cpu, tcb_t *thread,
							   interrupt_frame_t *frame, int status,
							   const char *how)
{
	if (!cpu || !thread || thread->state == TCB_ZOMBIE)
		return;
	if (thread->is_idle)
		kpanic(NULL, "sched: idle thread attempted to exit");

	if (frame)
		thread->rsp = (uint64_t)frame;
	thread->exit_status = status;
	thread->state = TCB_ZOMBIE;
	pcb_t *process = thread->process;
	pid_t exited_pid = process ? process->pid : -1;
	/*
	 * Unix-like orphan handling: when a userspace process exits, its living
	 * children become children of init (PID 1).  Kernel/internal tasks and
	 * PID 1 itself remain root-owned with PPID 0.
	 */
	pid_t new_parent = (exited_pid > 1) ? 1 : 0;
	process_exit_action_t action = process_remove_thread(thread, status);
	if (action != PROCESS_EXIT_NONE)
		process_reparent_children(exited_pid, new_parent);
	thread->reap_process = action == PROCESS_EXIT_REAP;
	atomic_fetch_sub_explicit(&cpu->sched_load, 1, memory_order_release);
	thread_queue_reap(cpu, thread);
	if (action == PROCESS_EXIT_ZOMBIE)
		process_note_zombie(thread->process);

	log_trace("sched", "tid=%d pid=%d exited%s on cpu%u status=%d", thread->tid,
			  thread->process ? thread->process->pid : -1, how ? how : "",
			  cpu->cpu_index, status);
}

static void thread_finish_current_locked(cpu_local_t *cpu,
									 interrupt_frame_t *frame, int status,
									 const char *how)
{
	thread_finish_locked(cpu, cpu ? cpu->current_thread : NULL, frame, status,
					  how);
}

static void thread_bootstrap(tcb_t *thread)
{
	assert(thread && thread->entry);
	log_trace("sched", "tid=%d pid=%d entering %s", thread->tid,
			  thread->process->pid, thread->name);
	sti();
	thread->entry(thread->arg);
	sched_exit();
}

static void idle_entry(void *arg)
{
	cpu_local_t *cpu = arg;
	log_trace("sched", "cpu%u idle thread online", cpu->cpu_index);
	sti();
	for (;;)
		hlt();
}

static tcb_t *thread_alloc(pcb_t *process, const char *name,
						   thread_entry_t entry, void *arg, bool idle)
{
	tcb_t *thread = kzalloc(sizeof(tcb_t));
	if (!thread)
		return NULL;

	thread->tid = idle ? 0 : (tid_t)id_alloc(&tid_pool);
	name_set(thread->name, sizeof(thread->name), name ? name : "thread");
	thread->state = TCB_READY;
	thread->entry = entry;
	thread->arg = arg;
	thread->process = process;
	thread->is_idle = idle;
	thread->kstack_top = kstack_alloc(SCHED_KERNEL_STACK_SIZE);
	thread->kstack_base = thread->kstack_top - SCHED_KERNEL_STACK_SIZE;
	frame_init_kernel(thread, thread_bootstrap);
	if (!process_add_thread(process, thread)) {
		kstack_free(thread->kstack_base, thread->kstack_top);
		if (!idle)
			id_free(&tid_pool, thread->tid);
		kfree(thread);
		return NULL;
	}
	return thread;
}

void sched_cpu_init(cpu_local_t *cpu)
{
	assert(cpu);

	uint64_t flags = irq_save();
	spinlock_init(&cpu->runq_lock);
	cpu->runq_head = NULL;
	cpu->runq_tail = NULL;
	cpu->current_thread = NULL;
	cpu->idle_thread = NULL;
	spinlock_init(&cpu->reap_lock);
	cpu->reap_head = NULL;
	atomic_store_explicit(&cpu->sched_load, 0, memory_order_release);
	atomic_store_explicit(&cpu->sched_ready, true, memory_order_release);
	irq_restore(flags);

	log_trace("sched", "cpu%u run queue initialized", cpu->cpu_index);
}

void sched_init(void)
{
	uint64_t flags = irq_save();
	spinlock_acquire(&sched_lock);

	if (atomic_load_explicit(&sched_ready, memory_order_acquire)) {
		spinlock_release(&sched_lock);
		irq_restore(flags);
		return;
	}

	cpu_local_t *cpu = get_cpu_local();
	if (!atomic_load_explicit(&cpu->sched_ready, memory_order_acquire))
		sched_cpu_init(cpu);

	pcb_t *idle_process = process_alloc_id("idle", _lyr_kernel_vas, 0);
	if (!idle_process)
		kpanic(NULL, "sched: failed to allocate idle process");
	process_link_locked(idle_process);

	pcb_t *kernel = process_alloc("lyr-kernel", _lyr_kernel_vas);
	if (!kernel)
		kpanic(NULL, "sched: failed to allocate kernel process");
	process_link_locked(kernel);

	tcb_t *bootstrap = kzalloc(sizeof(tcb_t));
	if (!bootstrap)
		kpanic(NULL, "sched: failed to allocate bootstrap TCB");

	bootstrap->tid = (tid_t)id_alloc(&tid_pool);
	name_set(bootstrap->name, sizeof(bootstrap->name), "bootstrap");
	bootstrap->state = TCB_RUNNING;
	bootstrap->mode = TCB_MODE_KERNEL;
	bootstrap->kstack_top = _lyr_kstack_top;
	bootstrap->process = kernel;
	bootstrap->cpu = cpu;
	cpu->current_thread = bootstrap;
	atomic_fetch_add_explicit(&cpu->sched_load, 1, memory_order_relaxed);
	process_add_thread(kernel, bootstrap);

	for (uint32_t i = 0; i < cpu_count; i++) {
		cpu_local_t *idle_cpu = &cpu_locals[i];
		char idle_name[32] = "idle";
		tcb_t *idle =
			thread_alloc(idle_process, idle_name, idle_entry, idle_cpu, true);
		if (!idle)
			kpanic(NULL, "sched: failed to allocate idle thread for cpu%u", i);
		idle->cpu = idle_cpu;
		idle_cpu->idle_thread = idle;
		if (idle_cpu != cpu && idle_cpu->current_thread == NULL)
			idle_cpu->current_thread = idle;
		log_trace("sched", "cpu%u idle tid=%d ready", i, idle->tid);
	}

	atomic_store_explicit(&sched_ready, true, memory_order_release);
	apic_timer_init(SCHED_TIMER_HZ);

	spinlock_release(&sched_lock);
	irq_restore(flags);
}

int sched_is_initialized(void)
{
	return atomic_load_explicit(&sched_ready, memory_order_acquire);
}

pcb_t *sched_process_create(const char *name, vas_t *vas)
{
	if (!sched_is_initialized())
		return NULL;

	pcb_t *process = process_alloc(name, vas);
	if (!process)
		return NULL;

	uint64_t flags = irq_save();
	spinlock_acquire(&sched_lock);
	process_link_locked(process);
	spinlock_release(&sched_lock);
	irq_restore(flags);

	log_trace("sched", "created process pid=%d name=%s pml4=0x%llx",
			  process->pid, process->name, (uint64_t)process->pml4);
	return process;
}

tcb_t *sched_create_thread(pcb_t *process, const char *name,
						   thread_entry_t entry, void *arg)
{
	if (!sched_is_initialized() || !process || !entry ||
		atomic_load_explicit(&process->dying, memory_order_acquire))
		return NULL;

	return sched_create_thread_on_cpu(process, name, entry, arg,
									  least_loaded_cpu());
}

tcb_t *sched_create_thread_on_cpu(pcb_t *process, const char *name,
								  thread_entry_t entry, void *arg,
								  cpu_local_t *cpu)
{
	if (!sched_is_initialized() || !process || !entry || !cpu ||
		atomic_load_explicit(&process->dying, memory_order_acquire))
		return NULL;

	uint64_t flags = irq_save();
	tcb_t *thread = thread_alloc(process, name, entry, arg, false);
	if (!thread) {
		irq_restore(flags);
		return NULL;
	}

	spinlock_acquire(&cpu->runq_lock);
	runq_push_locked(cpu, thread);
	atomic_fetch_add_explicit(&cpu->sched_load, 1, memory_order_release);
	spinlock_release(&cpu->runq_lock);
	irq_restore(flags);

	log_trace("sched", "created tid=%d pid=%d (%s) on cpu%u load=%u",
			  thread->tid, process->pid, thread->name, cpu->cpu_index,
			  atomic_load(&cpu->sched_load));
	return thread;
}

tcb_t *sched_create_user_thread(pcb_t *process, const char *name, uint64_t rip,
								uint64_t user_rsp)
{
	if (!sched_is_initialized() || !process || !rip || !user_rsp ||
		atomic_load_explicit(&process->dying, memory_order_acquire))
		return NULL;

	cpu_local_t *cpu = least_loaded_cpu();
	if (!cpu)
		return NULL;

	uint64_t flags = irq_save();
	tcb_t *thread = thread_alloc(process, name, NULL, NULL, false);
	if (!thread) {
		irq_restore(flags);
		return NULL;
	}

	frame_init_user(thread, rip, user_rsp);

	spinlock_acquire(&cpu->runq_lock);
	runq_push_locked(cpu, thread);
	atomic_fetch_add_explicit(&cpu->sched_load, 1, memory_order_release);
	spinlock_release(&cpu->runq_lock);
	irq_restore(flags);

	log_trace(
		"sched",
		"created user tid=%d pid=%d (%s) on cpu%u rip=0x%llx rsp=0x%llx load=%u",
		thread->tid, process->pid, thread->name, cpu->cpu_index, rip, user_rsp,
		atomic_load(&cpu->sched_load));
	return thread;
}


tcb_t *sched_fork_thread(pcb_t *process, const char *name,
                         interrupt_frame_t *parent_frame)
{
	if (!sched_is_initialized() || !process || !parent_frame ||
		atomic_load_explicit(&process->dying, memory_order_acquire))
		return NULL;

	cpu_local_t *cpu = least_loaded_cpu();
	if (!cpu)
		return NULL;

	uint64_t flags = irq_save();
	tcb_t *thread = thread_alloc(process, name, NULL, NULL, false);
	if (!thread) {
		irq_restore(flags);
		return NULL;
	}

	interrupt_frame_t *child_frame =
		(interrupt_frame_t *)(thread->kstack_top - sizeof(interrupt_frame_t));
	memcpy(child_frame, parent_frame, sizeof(*child_frame));
	child_frame->rax = 0;
	child_frame->cr3 = (uint64_t)process->pml4;
	thread->rsp = (uint64_t)child_frame;
	thread->mode = TCB_MODE_USER;
	thread->fs_base = sched_current() ? sched_current()->fs_base : 0;
	thread->user_entry_rsp = child_frame->rsp;

	spinlock_acquire(&cpu->runq_lock);
	runq_push_locked(cpu, thread);
	atomic_fetch_add_explicit(&cpu->sched_load, 1, memory_order_release);
	spinlock_release(&cpu->runq_lock);
	irq_restore(flags);

	log_trace("sched", "forked user tid=%d pid=%d (%s) on cpu%u rip=0x%llx rsp=0x%llx load=%u",
			  thread->tid, process->pid, thread->name, cpu->cpu_index,
			  child_frame->rip, child_frame->rsp, atomic_load(&cpu->sched_load));
	return thread;
}

tcb_t *sched_current(void)
{
	cpu_local_t *cpu = get_cpu_local();
	return cpu ? cpu->current_thread : NULL;
}


static void process_copy_info_locked(const pcb_t *process, sched_process_info_t *out)
{
	memset(out, 0, sizeof(*out));
	out->pid = process->pid;
	out->ppid = process->ppid;
	memcpy(out->name, process->name, sizeof(out->name));
	out->name[sizeof(out->name) - 1] = '\0';
	out->zombie = process->zombie;
	out->dying = atomic_load_explicit(&process->dying, memory_order_acquire);
	out->kernel = process->vas == _lyr_kernel_vas || !process->owns_vas;
	/*
	 * Unix-like supervision marker: user processes with a userspace parent
	 * are waitable/reapable by that parent, normally init(1), a shell, or a
	 * service supervisor. Kernel/root tasks remain unsupervised.
	 */
	out->supervised = !out->kernel && process->ppid > 0;
	out->exit_status = process->exit_status;
	out->thread_count = atomic_load_explicit(&process->thread_count,
										   memory_order_acquire);
	out->ruid = process->ruid;
	out->euid = process->euid;
	out->rgid = process->rgid;
	out->egid = process->egid;
	memcpy(out->cwd, sched_process_cwd(process), sizeof(out->cwd));
	out->cwd[sizeof(out->cwd) - 1] = '\0';
}

bool sched_process_get_info(pid_t pid, sched_process_info_t *out)
{
	if (!out)
		return false;

	bool found = false;
	uint64_t flags = irq_save();

	spinlock_acquire(&sched_lock);
	for (pcb_t *process = process_list; process; process = process->next) {
		if (process->pid == pid) {
			process_copy_info_locked(process, out);
			found = true;
			break;
		}
	}
	spinlock_release(&sched_lock);
	irq_restore(flags);

	return found;
}

bool sched_process_get_nth(size_t index, sched_process_info_t *out)
{
	if (!out)
		return false;

	bool found = false;
	uint64_t flags = irq_save();

	spinlock_acquire(&sched_lock);
	for (pcb_t *process = process_list; process; process = process->next) {
		if (process->pid <= 0)
			continue;
		if (index-- != 0)
			continue;
		process_copy_info_locked(process, out);
		found = true;
		break;
	}
	spinlock_release(&sched_lock);
	irq_restore(flags);

	return found;
}

bool sched_process_exists(pid_t pid)
{
	bool found = false;
	uint64_t flags = irq_save();

	spinlock_acquire(&sched_lock);
	for (pcb_t *process = process_list; process; process = process->next) {
		if (process->pid == pid) {
			found = true;
			break;
		}
	}
	spinlock_release(&sched_lock);
	irq_restore(flags);

	return found;
}

int sched_process_signal(pcb_t *sender, pid_t pid, int signal)
{
	if (!sender)
		return -EBADF;

	if (pid <= 0)
		return -ENOSYS;

	if (signal < 0 || signal > 64)
		return -EINVAL;

	uint64_t flags = irq_save();
	pcb_t *target = NULL;

	spinlock_acquire(&sched_lock);
	for (pcb_t *process = process_list; process; process = process->next) {
		if (process->pid == pid) {
			target = process;
			break;
		}
	}

	if (!target) {
		spinlock_release(&sched_lock);
		irq_restore(flags);
		return -ESRCH;
	}

	if (!process_signal_allowed(sender, target)) {
		spinlock_release(&sched_lock);
		irq_restore(flags);
		return -EPERM;
	}

	if (signal == 0) {
		spinlock_release(&sched_lock);
		irq_restore(flags);
		return 0;
	}

	if (signal != SCHED_SIGKILL && signal != SCHED_SIGTERM) {
		spinlock_release(&sched_lock);
		irq_restore(flags);
		return -ENOSYS;
	}

	if (process_is_kernel_owned(target)) {
		spinlock_release(&sched_lock);
		irq_restore(flags);
		return -EPERM;
	}

	target->exit_status = process_kill_status(signal);
	atomic_store_explicit(&target->dying, true, memory_order_release);

	log_trace("sched", "pid=%d signalled by pid=%d signal=%d", pid,
			  sender->pid, signal);

	spinlock_release(&sched_lock);
	irq_restore(flags);
	return 0;
}

int sched_process_wait(pcb_t *parent, pid_t pid, int options, pid_t *pid_out,
					   int *status_out)
{
	if (!parent || !pid_out || !status_out)
		return -EINVAL;

	/* Only WNOHANG is meaningful until job control exists. */
	if (options & ~1)
		return -EINVAL;

	if (pid == 0 || pid < -1)
		return -ENOSYS;

	pcb_t *zombie = NULL;
	bool have_child = false;
	uint64_t flags = irq_save();

	spinlock_acquire(&sched_lock);
	for (pcb_t *child = process_list; child; child = child->next) {
		if (child->ppid != parent->pid || !waitpid_match(pid, child))
			continue;

		have_child = true;
		if (child->zombie) {
			zombie = child;
			process_unlink_locked(child);
			break;
		}
	}
	spinlock_release(&sched_lock);
	irq_restore(flags);

	if (!have_child)
		return -ECHILD;

	if (!zombie) {
		if (options & 1) {
			*pid_out = 0;
			*status_out = 0;
			return 0;
		}

		return -EAGAIN;
	}

	*pid_out = zombie->pid;
	*status_out = (zombie->exit_status & 0xff) << 8;
	process_destroy(zombie);
	return 0;
}

const char *sched_process_cwd(const pcb_t *process)
{
	if (!process || process->cwd[0] == '\0')
		return "/";
	return process->cwd;
}

int sched_process_setcwd(pcb_t *process, const char *path)
{
	if (!process || !path || path[0] != '/')
		return -EINVAL;

	size_t len = strlen(path);
	if (len == 0 || len >= sizeof(process->cwd))
		return -ENAMETOOLONG;

	spinlock_acquire(&process->lock);
	memcpy(process->cwd, path, len + 1);
	spinlock_release(&process->lock);
	return 0;
}

void sched_process_copy_cwd(pcb_t *dst, const pcb_t *src)
{
	if (!dst || !src)
		return;

	spinlock_acquire(&dst->lock);
	memcpy(dst->cwd, sched_process_cwd(src), sizeof(dst->cwd));
	dst->cwd[sizeof(dst->cwd) - 1] = '\0';
	spinlock_release(&dst->lock);
}

bool sched_reap_pending(void)
{
	bool pending =
		atomic_load_explicit(&reap_inflight, memory_order_acquire) != 0;
	uint64_t flags = irq_save();

	for (uint32_t i = 0; i < cpu_count; i++) {
		cpu_local_t *cpu = &cpu_locals[i];

		spinlock_acquire(&cpu->reap_lock);
		if (cpu->reap_head)
			pending = true;
		spinlock_release(&cpu->reap_lock);

		if (pending)
			break;
	}

	irq_restore(flags);
	return pending;
}

void sched_idle_loop(void)
{
	while (!sched_is_initialized())
		__asm__ volatile("pause" ::: "memory");

	cli();
	cpu_local_t *cpu = get_cpu_local();
	if (!cpu || !cpu->idle_thread)
		kpanic(NULL, "sched_idle_loop: missing idle thread");

	apic_timer_init(SCHED_TIMER_HZ);
	switch_to_thread(cpu, cpu->idle_thread);
	log_trace("sched", "cpu%u entering idle loop", cpu->cpu_index);
	sched_iret_to_frame(thread_frame(cpu->idle_thread));
}

void sched_prepare_user(tcb_t *thread, uint64_t rip, uint64_t user_rsp)
{
	if (!thread || !rip || !user_rsp)
		return;

	uint64_t flags = irq_save();
	frame_init_user(thread, rip, user_rsp);
	log_trace("sched", "tid=%d pid=%d prepared user rip=0x%llx rsp=0x%llx",
			  thread->tid, thread->process->pid, rip, user_rsp);
	irq_restore(flags);
}

void sched_enter_user(uint64_t rip, uint64_t user_rsp)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process || !thread->process->vas)
		kpanic(NULL, "sched_enter_user: current thread has no process VAS");

	log_trace("sched", "tid=%d pid=%d dropping to user rip=0x%llx rsp=0x%llx",
			  thread->tid, thread->process->pid, rip, user_rsp);
	cli();
	thread->mode = TCB_MODE_USER;
	gdt_set_kernel_stack(thread->kstack_top);
	sched_switch_to_user(thread->process->pml4, rip, user_rsp);
}

interrupt_frame_t *sched_tick(interrupt_frame_t *frame)
{
	if (!sched_is_initialized() || !frame)
		return frame;

	cpu_local_t *cpu = get_cpu_local();
	if (!cpu || !atomic_load_explicit(&cpu->sched_ready, memory_order_acquire))
		return frame;

	sched_reap_current_cpu();

	if (!spinlock_try_acquire(&cpu->runq_lock))
		return frame;

	interrupt_frame_t *next_frame = schedule_locked(cpu, frame, true);
	spinlock_release(&cpu->runq_lock);
	return next_frame;
}

interrupt_frame_t *sched_syscall_exit(interrupt_frame_t *frame, int status)
{
	if (!sched_is_initialized() || !frame)
		return frame;

	cpu_local_t *cpu = get_cpu_local();
	if (!cpu)
		return frame;

	spinlock_acquire(&cpu->runq_lock);
	thread_finish_current_locked(cpu, frame, status, " via syscall");

	interrupt_frame_t *next_frame = schedule_locked(cpu, frame, false);
	spinlock_release(&cpu->runq_lock);
	return next_frame;
}

void sched_thread_exit(int status)
{
	cli();
	cpu_local_t *cpu = get_cpu_local();
	if (cpu) {
		spinlock_acquire(&cpu->runq_lock);
		thread_finish_current_locked(cpu, NULL, status, "");
		spinlock_release(&cpu->runq_lock);
	}
	sti();

	for (;;)
		hlt();
}

void sched_exit(void)
{
	sched_thread_exit(0);
}
