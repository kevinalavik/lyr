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

static inline uint8_t inb(uint16_t port)
{
	uint8_t value;
	__asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

#endif // _LYR_CPU_INSTR_H