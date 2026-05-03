#ifndef _LYR_SYS_SMP_H
#define _LYR_SYS_SMP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <limine.h>
#include <sync/spinlock.h>

#define MAX_CPUS 256

struct tcb;

typedef struct cpu_local {
	uint32_t lapic_id;
	uint32_t cpu_index;
	atomic_bool ready;
	atomic_bool sched_ready;
	spinlock_t runq_lock;
	struct tcb *runq_head;
	struct tcb *runq_tail;
	struct tcb *current_thread;
	struct tcb *idle_thread;
	spinlock_t reap_lock;
	struct tcb *reap_head;
	atomic_uint sched_load;
} cpu_local_t;

extern uint32_t bootstrap_lapic_id;
extern cpu_local_t cpu_locals[MAX_CPUS];
extern uint32_t cpu_count;

void smp_init(struct limine_mp_response *mp);
cpu_local_t *get_cpu_local(void);

#endif
