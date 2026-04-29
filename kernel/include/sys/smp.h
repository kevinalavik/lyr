#ifndef _LYR_SYS_SMP_H
#define _LYR_SYS_SMP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <limine.h>

#define MAX_CPUS 256

typedef struct {
	uint32_t lapic_id;
	uint32_t cpu_index;
	atomic_bool ready;
} cpu_local_t;

extern uint32_t bootstrap_lapic_id;
extern cpu_local_t cpu_locals[MAX_CPUS];
extern uint32_t cpu_count;

void smp_init(struct limine_mp_response *mp);
cpu_local_t *get_cpu_local(void);

#endif
