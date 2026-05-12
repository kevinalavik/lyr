#include <cpu/gdt.h>
#include <lib/string.h>
#include <sys/smp.h>
/*
Access Byte
-----------
   bit 7 : Present
   bits 6-5 : DPL
   bit 4 : Descriptor type (1 = code/data)
   bit 3 : Executable (1 = code)
   bit 2 : Direction/Conforming
   bit 1 : Read/Write
   bit 0 : Accessed
*/

#define GDT_P 0b10000000
#define GDT_DPL0 0b00000000
#define GDT_DPL3 0b01100000
#define GDT_S 0b00010000

#define GDT_CODE 0b00001000
#define GDT_DATA 0b00000000

#define GDT_RW 0b00000010
#define GDT_A 0b00000001

#define GDT_KERNEL_CODE (GDT_P | GDT_DPL0 | GDT_S | GDT_CODE | GDT_RW)
#define GDT_KERNEL_DATA (GDT_P | GDT_DPL0 | GDT_S | GDT_DATA | GDT_RW)

#define GDT_USER_CODE (GDT_P | GDT_DPL3 | GDT_S | GDT_CODE | GDT_RW)
#define GDT_USER_DATA (GDT_P | GDT_DPL3 | GDT_S | GDT_DATA | GDT_RW)

#define GDT_FLAG_GRAN_4K 0b10000000
#define GDT_FLAG_64BIT 0b00100000

#define _ENTRY(base, limit, access, flags)                         \
	((gdt_descriptor_t){                                           \
		(uint16_t)((limit) & 0xFFFF), (uint16_t)((base) & 0xFFFF), \
		(uint8_t)(((base) >> 16) & 0xFF), (uint8_t)(access),       \
		(uint8_t)((((limit) >> 16) & 0x0F) | ((flags) & 0xF0)),    \
		(uint8_t)(((base) >> 24) & 0xFF) })

static const gdt_t gdt_template = { .entries = {
										_ENTRY(0, 0, 0, 0),

										/* kernel */
										_ENTRY(0, 0xFFFF, GDT_KERNEL_CODE,
											   GDT_FLAG_GRAN_4K |
												   GDT_FLAG_64BIT),

										_ENTRY(0, 0xFFFF, GDT_KERNEL_DATA,
											   GDT_FLAG_GRAN_4K),

										/* user */
										_ENTRY(0, 0xFFFF, GDT_USER_CODE,
											   GDT_FLAG_GRAN_4K |
												   GDT_FLAG_64BIT),

										_ENTRY(0, 0xFFFF, GDT_USER_DATA,
											   GDT_FLAG_GRAN_4K),

										/* TSS, filled per CPU. */
										_ENTRY(0, 0, 0, 0),
										_ENTRY(0, 0, 0, 0),
									} };

static gdt_t cpu_gdt[MAX_CPUS];
static gdtr_t cpu_gdtr[MAX_CPUS];

typedef struct {
	uint32_t reserved0;
	uint64_t rsp[3];
	uint64_t reserved1;
	uint64_t ist[7];
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t iopb;
} __attribute__((packed)) tss_t;

static tss_t cpu_tss[MAX_CPUS];

static void gdt_set_tss_descriptor(uint32_t cpu_index, tss_t *tss)
{
	if (cpu_index >= MAX_CPUS)
		cpu_index = 0;

	uint64_t base = (uint64_t)tss;
	uint64_t limit = sizeof(*tss) - 1;
	uint64_t low = 0;

	low |= limit & 0xFFFF;
	low |= (base & 0xFFFF) << 16;
	low |= ((base >> 16) & 0xFF) << 32;
	low |= 0x89ULL << 40;
	low |= ((limit >> 16) & 0x0F) << 48;
	low |= ((base >> 24) & 0xFF) << 56;

	uint64_t *raw = (uint64_t *)cpu_gdt[cpu_index].entries;
	raw[5] = low;
	raw[6] = base >> 32;
}

void gdt_init()
{
	gdt_init_cpu(0);
}

void gdt_init_cpu(uint32_t cpu_index)
{
	if (cpu_index >= MAX_CPUS)
		cpu_index = 0;

	memcpy(&cpu_gdt[cpu_index], &gdt_template, sizeof(gdt_template));
	cpu_gdtr[cpu_index].limit = sizeof(cpu_gdt[cpu_index].entries) - 1;
	cpu_gdtr[cpu_index].base = (uint64_t)&cpu_gdt[cpu_index].entries;

	__asm__ volatile("lgdt %[g]\n"
					 "pushq $0x08\n"
					 "lea 1f(%%rip), %%rax\n"
					 "pushq %%rax\n"
					 "lretq\n"
					 "1:\n"
					 "mov $0x10, %%ax\n"
					 "mov %%ax, %%ds\n"
					 "mov %%ax, %%es\n"
					 "mov %%ax, %%ss\n"
					 "mov %%ax, %%fs\n"
					 "mov %%ax, %%gs\n"
					 :
					 : [g] "m"(cpu_gdtr[cpu_index])
					 : "rax", "memory");
}

void gdt_tss_init_cpu(uint32_t cpu_index, uint64_t rsp0)
{
	if (cpu_index >= MAX_CPUS)
		cpu_index = 0;

	tss_t *tss = &cpu_tss[cpu_index];
	memset(tss, 0, sizeof(*tss));
	tss->rsp[0] = rsp0;
	tss->iopb = sizeof(*tss);
	cpu_locals[cpu_index].syscall_rsp0 = rsp0;
	gdt_set_tss_descriptor(cpu_index, tss);

	__asm__ volatile("ltr %%ax" ::"a"((uint16_t)0x28) : "memory");
}

void gdt_tss_init(uint64_t rsp0)
{
	gdt_tss_init_cpu(0, rsp0);
}

void gdt_set_kernel_stack(uint64_t rsp0)
{
	cpu_local_t *cpu = get_cpu_local();
	uint32_t index = cpu ? cpu->cpu_index : 0;
	if (index >= MAX_CPUS)
		index = 0;
	if (cpu)
		cpu->syscall_rsp0 = rsp0;
	cpu_tss[index].rsp[0] = rsp0;
}
