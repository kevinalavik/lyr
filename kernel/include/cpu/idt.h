#ifndef _LYR_CPU_IDT_H
#define _LYR_CPU_IDT_H

#include <stdint.h>

typedef struct {
	uint16_t base_low;
	uint16_t codeseg;
	uint8_t ist;
	uint8_t flags;
	uint16_t base_mid;
	uint32_t base_high;
	uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

typedef struct {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed)) idtr_t;

typedef struct {
	uint64_t es;
	uint64_t ds;

	uint64_t cr0;
	uint64_t cr2;
	uint64_t cr3;
	uint64_t cr4;

	uint64_t rax;
	uint64_t rbx;
	uint64_t rcx;
	uint64_t rdx;
	uint64_t rbp;
	uint64_t rdi;
	uint64_t rsi;
	uint64_t r8;
	uint64_t r9;
	uint64_t r10;
	uint64_t r11;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;

	uint64_t vector;
	uint64_t err;

	uint64_t rip;
	uint64_t cs;
	uint64_t rflags;
	uint64_t rsp;
	uint64_t ss;
} __attribute__((packed)) interrupt_frame_t;

#define IRQ_BASE 0x20

typedef interrupt_frame_t *(*irq_callback)(interrupt_frame_t *);

typedef struct {
	void *ctx;
	irq_callback callback;
} irq_handler_t;

void idt_init(void);
void idt_set_desc(idt_entry_t *desc, uint64_t offset, uint8_t type,
				  uint8_t dpl);

void irq_install(uint8_t irq, irq_callback callback, interrupt_frame_t *ctx,
				 uint8_t lapic_id);

#endif
