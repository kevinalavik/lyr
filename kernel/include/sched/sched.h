#ifndef _LYR_SCHED_SCHED_H
#define _LYR_SCHED_SCHED_H

#include <cpu/idt.h>
#include <dev/time.h>
#include <fs/vfs.h>
#include <mm/paging.h>
#include <mm/vmm.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sync/spinlock.h>
#include <sys/smp.h>

#define SCHED_TIMER_HZ 1000
#define SCHED_KERNEL_STACK_SIZE (64 * 1024ULL)
#define SCHED_FILE_MAX 128
#define SCHED_CWD_MAX 512
#define SCHED_NSIG 64

#define SCHED_SIG_BLOCK 0
#define SCHED_SIG_UNBLOCK 1
#define SCHED_SIG_SETMASK 2

typedef void (*sched_sighandler_t)(int);

typedef struct sched_sigaction {
	uint64_t handler;
	uint64_t flags;
	uint64_t restorer;
	uint64_t mask;
} sched_sigaction_t;

typedef int32_t pid_t;
typedef int32_t tid_t;

typedef enum {
	TCB_READY = 0,
	TCB_RUNNING,
	TCB_BLOCKED,
	TCB_ZOMBIE,
} tcb_state_t;

typedef enum {
	TCB_MODE_KERNEL = 0,
	TCB_MODE_USER,
} tcb_mode_t;

typedef void (*thread_entry_t)(void *);

typedef struct sched_waitq {
	atomic_uint seq;
} sched_waitq_t;

typedef struct pcb {
	pid_t pid;
	pid_t ppid;
	char name[32];
	ptable_t *pml4;
	vas_t *vas;
	bool owns_vas;
	atomic_bool dying;
	bool zombie;
	int exit_status;
	spinlock_t lock;
	struct tcb *threads;
	atomic_uint thread_count;
	vfs_cred_t cred;
	vfs_uid_t ruid;
	vfs_gid_t rgid;
	vfs_uid_t suid;
	vfs_gid_t sgid;
	vfs_uid_t euid;
	vfs_gid_t egid;
	pid_t pgid;
	pid_t sid;
	int controlling_tty;
	char cwd[SCHED_CWD_MAX];
	vfs_file_t *files[SCHED_FILE_MAX];
	uint32_t fd_flags[SCHED_FILE_MAX];
	sched_sigaction_t sigactions[SCHED_NSIG + 1];
	uint64_t pending_signals;
	/* job-control state (protected by sched_lock) */
	bool
		stopped; /* process is currently stopped by SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU */
	int stop_signal; /* which signal caused the stop */
	bool stop_reported; /* WIFSTOPPED status already returned to waitpid */
	bool continued; /* process resumed via SIGCONT since last waitpid */
	/* Bumped under sched_lock whenever the child's wait-state changes so
	 * that a parent sleeping in waitpid() wakes on the next scheduler tick. */
	atomic_uint child_event;
	sched_waitq_t child_waitq;
	struct pcb *next;
} pcb_t;

/* POSIX waitpid() option flags */
#define SCHED_WNOHANG 1
#define SCHED_WUNTRACED 2
#define SCHED_WCONTINUED 8

typedef struct sched_process_info {
	pid_t pid;
	pid_t ppid;
	char name[32];
	bool zombie;
	bool dying;
	bool kernel;
	bool supervised;
	int exit_status;
	unsigned thread_count;
	vfs_uid_t ruid;
	vfs_uid_t euid;
	vfs_gid_t rgid;
	vfs_gid_t egid;
	char cwd[SCHED_CWD_MAX];
} sched_process_info_t;

typedef struct tcb {
	tid_t tid;
	char name[32];
	tcb_state_t state;
	tcb_mode_t mode;
	bool is_idle;
	bool sleep_hint;
	bool reap_process;
	uint64_t fs_base;
	uint64_t user_entry_rsp;
	uint64_t signal_mask;
	uint64_t rsp;
	uint64_t kstack_base;
	uint64_t kstack_top;
	thread_entry_t entry;
	void *arg;
	int exit_status;
	pcb_t *process;
	cpu_local_t *cpu;
	struct tcb *process_next;
	struct tcb *runq_next;
	struct tcb *reap_next;
} tcb_t;

void sched_cpu_init(cpu_local_t *cpu);
void sched_init(void);
int sched_is_initialized(void);

pcb_t *sched_process_create(const char *name, vas_t *vas);
void sched_process_discard(pcb_t *process);
tcb_t *sched_create_thread(pcb_t *process, const char *name,
						   thread_entry_t entry, void *arg);
tcb_t *sched_create_thread_on_cpu(pcb_t *process, const char *name,
								  thread_entry_t entry, void *arg,
								  cpu_local_t *cpu);
tcb_t *sched_create_user_thread(pcb_t *process, const char *name, uint64_t rip,
								uint64_t user_rsp);
tcb_t *sched_fork_thread(pcb_t *process, const char *name,
						 interrupt_frame_t *parent_frame);

tcb_t *sched_current(void);
bool sched_process_exists(pid_t pid);
bool sched_process_get_info(pid_t pid, sched_process_info_t *out);
bool sched_process_get_nth(size_t index, sched_process_info_t *out);
int sched_process_wait(pcb_t *parent, pid_t pid, int options, pid_t *pid_out,
					   int *status_out);
int sched_process_signal(pcb_t *sender, pid_t pid, int signal);
int sched_process_signal_group(pid_t pgid, int signal);
int sched_signal_action(pcb_t *process, int signal,
						const sched_sigaction_t *act,
						sched_sigaction_t *oldact);
int sched_signal_procmask(tcb_t *thread, int how, const uint64_t *set,
						  uint64_t *oldset);
int sched_signal_is_pending(tcb_t *thread);
interrupt_frame_t *sched_signal_deliver(interrupt_frame_t *frame);
interrupt_frame_t *sched_signal_return(interrupt_frame_t *frame);
interrupt_frame_t *sched_handle_user_exception(interrupt_frame_t *frame,
											   int signal, const char *reason);
int sched_process_getpgid(pcb_t *caller, pid_t pid, pid_t *pgid_out);
int sched_process_setpgid(pcb_t *caller, pid_t pid, pid_t pgid);
int sched_process_setsid(pcb_t *caller, pid_t *sid_out);
const char *sched_process_cwd(const pcb_t *process);
int sched_process_setcwd(pcb_t *process, const char *path);
void sched_process_copy_cwd(pcb_t *dst, const pcb_t *src);
const vfs_cred_t *sched_process_cred(const pcb_t *process);
void sched_process_set_parent(pcb_t *process, pid_t ppid);
void sched_process_set_name(pcb_t *process, const char *name);
void sched_thread_set_name(tcb_t *thread, const char *name);
void sched_process_copy_ids(pcb_t *dst, const pcb_t *src);
int sched_process_setuid(pcb_t *process, vfs_uid_t uid);
int sched_process_setgid(pcb_t *process, vfs_gid_t gid);
int sched_process_seteuid(pcb_t *process, vfs_uid_t uid);
int sched_process_setegid(pcb_t *process, vfs_gid_t gid);
void sched_map_kernel_mmio(uint64_t virt, uint64_t phys, uint64_t npages);
bool sched_reap_pending(void);
size_t sched_runnable_threads(void);
void sched_loadavg_task_counts(size_t *runnable_out, size_t *total_out);
void sched_loadavg_snapshot(uint64_t out[3]);
pid_t sched_last_pid(void);
void sched_waitq_init(sched_waitq_t *waitq);
unsigned sched_waitq_prepare(const sched_waitq_t *waitq);
void sched_waitq_wake_all(sched_waitq_t *waitq);
int sched_waitq_wait(sched_waitq_t *waitq, unsigned seq,
					 const time_timeout_t *timeout);
unsigned sched_io_wait_prepare(void);
int sched_io_wait(unsigned seq, const time_timeout_t *timeout);
void sched_io_wake_all(void);
void sched_sleep_hint_begin(void);
void sched_sleep_hint_end(void);
void sched_idle_loop(void) __attribute__((noreturn));
void sched_prepare_user(tcb_t *thread, uint64_t rip, uint64_t user_rsp);
void sched_enter_user(uint64_t rip, uint64_t user_rsp)
	__attribute__((noreturn));

interrupt_frame_t *sched_tick(interrupt_frame_t *frame);
interrupt_frame_t *sched_syscall_exit(interrupt_frame_t *frame, int status);
void sched_thread_exit(int status) __attribute__((noreturn));
void sched_exit(void) __attribute__((noreturn));

void process_setup_fds(pcb_t *process);

#endif /* _LYR_SCHED_SCHED_H */
