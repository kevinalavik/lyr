#include <sys/smp.h>
#include <cpu/instr.h>
#include <cpu/gdt.h>
#include <cpu/idt.h>
#include <mm/paging.h>
#include <sys/apic.h>
#include <debug/panic.h>
#include <debug/log.h>
#include <sched/sched.h>
#include <stdatomic.h>
#include <sys/syscall.h>

#define MSR_GS_BASE 0xC0000101
#define CPU_START_TIMEOUT 10000000UL

static atomic_int ap_go = 0;

uint32_t cpu_count = 0;
uint32_t bootstrap_lapic_id = 0;
atomic_uint started_cpus = 0;
cpu_local_t cpu_locals[MAX_CPUS];

static void cpu_enable_sse(void)
{
	uint64_t cr0 = read_cr0();
	uint64_t cr4 = read_cr4();

	cr0 &= ~(1ULL << 2);
	cr0 |= (1ULL << 1);
	cr4 |= (1ULL << 9) | (1ULL << 10);

	write_cr0(cr0);
	write_cr4(cr4);
	fninit();
}

static inline void set_cpu_local(cpu_local_t *cpu)
{
	wrmsr(MSR_GS_BASE, (uint64_t)cpu);
}

cpu_local_t *get_cpu_local(void)
{
	cpu_local_t *cpu = (cpu_local_t *)(uintptr_t)rdmsr(MSR_GS_BASE);
	if (cpu)
		return cpu;

	uint32_t id = lapic_read(0x0020) >> 24;
	for (uint32_t i = 0; i < cpu_count; i++)
		if (cpu_locals[i].lapic_id == id)
			return &cpu_locals[i];

	log_err("smp", "No CPU found with LAPIC ID %u", id);
	return NULL;
}

static void init_cpu(cpu_local_t *cpu)
{
	gdt_init_cpu(cpu->cpu_index);
	cpu_enable_sse();
	set_cpu_local(cpu);
	gdt_tss_init_cpu(cpu->cpu_index, 0);
	idt_init();
	syscall_init();
	write_cr3((uint64_t)kernel_ptable);
	apic_cpu_init(cpu->cpu_index);
	if (!atomic_load_explicit(&cpu->sched_ready, memory_order_acquire))
		sched_cpu_init(cpu);
	log_trace("smp", "hello from cpu %d", cpu->cpu_index);
}

void smp_entry(struct limine_mp_info *smp_info)
{
	uint32_t lapic_id = smp_info->lapic_id;
	cpu_local_t *cpu = NULL;

	for (uint32_t i = 0; i < cpu_count; i++) {
		if (cpu_locals[i].lapic_id == lapic_id) {
			cpu = &cpu_locals[i];
			break;
		}
	}
	if (!cpu)
		kpanic(NULL, "AP: LAPIC ID %u not found in cpu_locals", lapic_id);

	init_cpu(cpu);

	atomic_store_explicit(&cpu->ready, true, memory_order_release);
	atomic_fetch_add_explicit(&started_cpus, 1, memory_order_relaxed);

	while (!atomic_load_explicit(&ap_go, memory_order_acquire))
		__asm__ volatile("pause" ::: "memory");

	sched_idle_loop();
}

void smp_init(struct limine_mp_response *mp)
{
	if (!mp)
		kpanic(NULL, "smp_init: no MP response from Limine");

	cli();
	atomic_store(&started_cpus, 0);
	atomic_store(&ap_go, 0);

	cpu_count = mp->cpu_count;
	if (cpu_count > MAX_CPUS)
		kpanic(NULL, "Too many CPUs: max %u, detected %u", MAX_CPUS, cpu_count);

	bootstrap_lapic_id = mp->bsp_lapic_id;
	log_debug("smp", "Detected %u CPU(s), BSP LAPIC ID: %u", cpu_count,
			  bootstrap_lapic_id);

	for (uint32_t i = 0; i < cpu_count; i++) {
		struct limine_mp_info *info = mp->cpus[i];
		cpu_locals[i].lapic_id = info->lapic_id;
		cpu_locals[i].cpu_index = i;
		atomic_init(&cpu_locals[i].ready, false);
	}

	for (uint32_t i = 0; i < cpu_count; i++) {
		struct limine_mp_info *info = mp->cpus[i];

		if (info->lapic_id == bootstrap_lapic_id) {
			init_cpu(&cpu_locals[i]);
			atomic_store_explicit(&cpu_locals[i].ready, true,
								  memory_order_release);
			atomic_fetch_add_explicit(&started_cpus, 1, memory_order_relaxed);
			log_trace("smp", "BSP (LAPIC %u, index %u) initialized.",
					  cpu_locals[i].lapic_id, cpu_locals[i].cpu_index);
		} else {
			atomic_store_explicit((_Atomic uintptr_t *)&info->goto_address,
								  (uintptr_t)smp_entry, memory_order_seq_cst);

			uint64_t timeout = 0;
			while (!atomic_load_explicit(&cpu_locals[i].ready,
										 memory_order_acquire) &&
				   timeout < CPU_START_TIMEOUT) {
				__asm__ volatile("pause" ::: "memory");
				timeout++;
			}
			if (!atomic_load_explicit(&cpu_locals[i].ready,
									  memory_order_acquire))
				kpanic(NULL, "CPU %u (LAPIC %u) timed out: %u/%u started", i,
					   info->lapic_id, atomic_load(&started_cpus), cpu_count);

			log_debug("smp", "CPU %u online (index %u).", info->lapic_id,
					  cpu_locals[i].cpu_index);
		}
	}

	if (atomic_load(&started_cpus) != cpu_count)
		kpanic(NULL, "Not all CPUs started: %u/%u", atomic_load(&started_cpus),
			   cpu_count);

	log_debug("smp", "All %u CPUs online.", cpu_count);
	atomic_store_explicit(&ap_go, 1, memory_order_release);
	sti();
}
