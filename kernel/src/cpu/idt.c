#include <cpu/idt.h>
#include <debug/log.h>
#include <debug/assert.h>
#include <debug/panic.h>

#define IDT_TRAP 0xF
#define IDT_INTERRUPT 0xE

const char *exception_str[32] = {
	"Division By Zero",
	"Debug",
	"NMI",
	"Breakpoint",
	"Overflow",
	"Bound Range Exceeded",
	"Invalid Opcode",
	"Device Not Available",
	"Double Fault",
	"Reserved", // coprocessor segment overrun
	"Invalid TSS",
	"Segment Not Present",
	"Stack-Segment Fault",
	"General Protection Fault",
	"Page Fault",
	"Reserved",
	"X87 Exception",
	"Alignment Check",
	"Machine Check",
	"SIMD Exception",
	"Virtualization Exception",
	"Control Protection",
	"Reserved",
	"Reserved",
	"Reserved",
	"Reserved",
	"Reserved",
	"Reserved",
	"Hypervisor Injection",
	"VMM Communication",
	"Security",
	"Reserved",
};

__attribute__((aligned(16))) idt_entry_t idt[256];
idtr_t idtr = { .limit = (uint16_t)((sizeof(idt_entry_t) * 256) - 1),
				.base = (uint64_t)&idt[0] };

extern void *isr_stubs[256];

void idt_init()
{
	for (int v = 0; v < 32; v++) {
		idt_set_desc(&idt[v], (uint64_t)isr_stubs[v], IDT_TRAP, 0);
	}
	for (int v = 32; v < 256; v++) {
		idt_set_desc(&idt[v], (uint64_t)isr_stubs[v], IDT_INTERRUPT, 0);
	}

	// syscall handler
	idt_set_desc(&idt[0x80], (uint64_t)isr_stubs[0x80], IDT_TRAP,
				 3); // DPL=3 for user access

	__asm__ volatile("lidt %0" ::"m"(idtr));
}

void idt_set_desc(idt_entry_t *desc, uint64_t offset, uint8_t type, uint8_t dpl)
{
	desc->base_low = offset & 0xFFFF;
	desc->codeseg = 0x08;
	desc->ist = 0;
	desc->flags = (1 << 7) | (dpl << 5) | (type);
	desc->base_mid = (offset >> 16) & 0xFFFF;
	desc->base_high = (offset >> 32) & 0xFFFFFFFF;
	desc->reserved = 0;
}

void isr_common_handler(interrupt_frame_t frame)
{
	if (frame.vector < 0x20) {
		kpanic(&frame, "%s", exception_str[frame.vector]);
	} else {
		log_warn("idt", "Unhandled interrupt: %u\n", frame.vector);
	}
}