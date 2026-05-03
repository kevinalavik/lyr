#ifndef _LYR_CPU_INSTR_H
#define _LYR_CPU_INSTR_H

#include <stdint.h>

static inline void cli()
{
	__asm__ volatile("cli" ::: "memory");
}

static inline void sti()
{
	__asm__ volatile("sti" ::: "memory");
}

static inline void hlt()
{
	__asm__ volatile("hlt");
}

static inline void nointloop()
{
	cli();
	for (;;)
		hlt();
}

static inline void outb(uint16_t port, uint8_t value)
{
	__asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value)
{
	__asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t value)
{
	__asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
	uint8_t value;
	__asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static inline uint16_t inw(uint16_t port)
{
	uint16_t value;
	__asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static inline uint32_t inl(uint16_t port)
{
	uint32_t value;
	__asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static inline uint64_t read_cr3(void)
{
	uint64_t val;
	__asm__ volatile("mov %%cr3, %0" : "=r"(val));
	return val;
}

static inline void write_cr3(uint64_t cr3)
{
	__asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
}

static inline uint64_t rdmsr(uint32_t msr)
{
	uint32_t lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val)
{
	__asm__ volatile("wrmsr" ::"c"(msr), "a"((uint32_t)val),
					 "d"((uint32_t)(val >> 32)));
}

static inline uint64_t read_rflags(void)
{
	uint64_t rflags;
	__asm__ volatile("pushfq; popq %0" : "=r"(rflags)::"memory");
	return rflags;
}

static inline int interrupts_enabled(void)
{
	/* IF = bit 9 */
	return (read_rflags() & (1ULL << 9)) != 0;
}

#endif // _LYR_CPU_INSTR_H
