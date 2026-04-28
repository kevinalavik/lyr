#include <cpu/idt.h>
#include <debug/log.h>
#include <debug/assert.h>
#include <debug/panic.h>
#include <stddef.h>
#include <sys/apic.h>

#define IDT_TRAP 0xF
#define IDT_INTERRUPT 0xE

static const char *_exception_str[32] = {
	"Division By Zero",
	"Debug",
	"NMI",
	"Breakpoint",
	"Overflow",
	"Bound Range Exceeded",
	"Invalid Opcode",
	"Device Not Available",
	"Double Fault",
	"Reserved",
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
idtr_t idtr = {
	.limit = (uint16_t)((sizeof(idt_entry_t) * 256) - 1),
	.base = (uint64_t)&idt[0],
};

extern void *isr_stubs[256];

/* irq handleing */
irq_handler_t irq_handlers[224] = { 0 };

void irq_install(uint8_t irq, irq_callback callback, interrupt_frame_t *ctx)
{
	if (irq > 16) {
		log_warn("apic", "Tried to install an IRQ handler for IRQ#%u.", irq);
		return;
	}

	irq_handler_t *h = &irq_handlers[irq];
	if (h->callback) {
		log_warn("irq",
				 "Overwriting IRQ callback 0x%llx for IRQ%u with 0x%llx.",
				 h->callback, irq, callback);
	}

	h->callback = callback;
	h->ctx = ctx;

	uint8_t cpu = 0; // bsp only for now;

	if (irq == 0)
		ioapic_write_red(irq, IRQ_BASE + irq, 0, 0, 0, 0xFF);
	else
		ioapic_write_red(irq, IRQ_BASE + irq, 0, 0, 0, (uint8_t)(1u << cpu));

	log_trace("irq", "Installed IRQ handler 0x%llx for IRQ%u.", callback, irq);
}

void irq_uninstall(uint8_t irq)
{
	if (irq > 16) {
		log_warn("irq", "Tried to uninstall an IRQ handler for IRQ#%u.", irq);
		return;
	}

	irq_handler_t *h = &irq_handlers[irq];

	h->callback = NULL;
	h->ctx = NULL;

	log_trace("irq", "Uninstalled IRQ handler for IRQ#%u.", irq);
}

void irq_dispatch(uint8_t irq)
{
	irq_handler_t *h = &irq_handlers[irq];
	if (!h->callback) {
		log_warn("irq", "Unhandled IRQ#%u.", irq);
		return;
	}

	h->callback(h->ctx);
}

/* end */

void idt_set_desc(idt_entry_t *desc, uint64_t offset, uint8_t type, uint8_t dpl)
{
	desc->base_low = offset & 0xFFFF;
	desc->codeseg = 0x08;
	desc->ist = 0;
	desc->flags = (1 << 7) | (dpl << 5) | type;
	desc->base_mid = (offset >> 16) & 0xFFFF;
	desc->base_high = (offset >> 32) & 0xFFFFFFFF;
	desc->reserved = 0;
}

void idt_init(void)
{
	for (int v = 0; v < 32; v++)
		idt_set_desc(&idt[v], (uint64_t)isr_stubs[v], IDT_TRAP, 0);

	for (int v = 32; v < 256; v++)
		idt_set_desc(&idt[v], (uint64_t)isr_stubs[v], IDT_INTERRUPT, 0);

	idt_set_desc(&idt[0x80], (uint64_t)isr_stubs[0x80], IDT_TRAP, 3);

	__asm__ volatile("lidt %0" ::"m"(idtr));
}

void isr_common_handler(interrupt_frame_t *frame)
{
	if (frame->vector < IRQ_BASE) {
		kpanic(frame, "%s", _exception_str[frame->vector]);
		return;
	}

	irq_dispatch(frame->vector - IRQ_BASE);
	apic_send_eoi();
}