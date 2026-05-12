#include <cpu/idt.h>
#include <debug/log.h>
#include <debug/assert.h>
#include <debug/panic.h>
#include <stddef.h>
#include <sys/syscall.h>
#include <sys/apic.h>
#include <sys/smp.h>
#include <sched/sched.h>
#include <mm/vmm.h>

#define IDT_TRAP 0xF
#define IDT_INTERRUPT 0xE

#define APIC_SPURIOUS_VECTOR 0xFF

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

#define SCHED_SIGILL 4
#define SCHED_SIGTRAP 5
#define SCHED_SIGBUS 7
#define SCHED_SIGFPE 8
#define SCHED_SIGSEGV 11

static int exception_signal(uint64_t vector)
{
	switch (vector) {
		case 0:
		case 16:
		case 19:
			return SCHED_SIGFPE;
		case 1:
		case 3:
			return SCHED_SIGTRAP;
		case 6:
			return SCHED_SIGILL;
		case 10:
		case 11:
		case 12:
		case 13:
		case 14:
		case 17:
		case 21:
			return SCHED_SIGSEGV;
		case 18:
			return SCHED_SIGBUS;
		default:
			return SCHED_SIGILL;
	}
}

__attribute__((aligned(16))) idt_entry_t idt[256];
idtr_t idtr = {
	.limit = (uint16_t)((sizeof(idt_entry_t) * 256) - 1),
	.base = (uint64_t)&idt[0],
};

extern void *isr_stubs[256];

irq_handler_t irq_handlers[224] = { 0 };

void irq_install(uint8_t irq, irq_callback callback, interrupt_frame_t *ctx,
				 uint8_t lapic_id)
{
	if (irq > 16) {
		log_warn("irq", "Tried to install handler for IRQ#%u (max 16).", irq);
		return;
	}

	irq_handler_t *h = &irq_handlers[irq];
	if (h->callback) {
		log_warn("irq", "Overwriting IRQ#%u callback 0x%llx with 0x%llx.", irq,
				 h->callback, callback);
	}

	h->callback = callback;
	h->ctx = ctx;

	ioapic_write_red(irq, IRQ_BASE + irq, IOAPIC_FIXED, IOAPIC_ACTIVE_HI,
					 IOAPIC_TRIGGER_EDGE, (uint8_t)lapic_id);

	log_trace("irq", "Installed IRQ#%u -> LAPIC %u (handler 0x%llx).", irq,
			  lapic_id, callback);
}

void irq_uninstall(uint8_t irq)
{
	if (irq > 16) {
		log_warn("irq", "Tried to uninstall handler for IRQ#%u (max 16).", irq);
		return;
	}

	irq_handler_t *h = &irq_handlers[irq];
	h->callback = NULL;
	h->ctx = NULL;

	log_trace("irq", "Uninstalled IRQ#%u handler.", irq);
}

interrupt_frame_t *irq_dispatch(uint8_t irq, interrupt_frame_t *frame)
{
	irq_handler_t *h = &irq_handlers[irq];
	if (!h->callback) {
		log_warn("irq", "Unhandled IRQ#%u.", irq);
		return frame;
	}
	interrupt_frame_t *next = h->callback(h->ctx ? h->ctx : frame);
	return next ? next : frame;
}

void idt_set_desc(idt_entry_t *desc, uint64_t offset, uint8_t type, uint8_t dpl)
{
	desc->base_low = offset & 0xFFFF;
	desc->codeseg = 0x08;
	desc->ist = 0;
	desc->flags = (uint8_t)((1 << 7) | (dpl << 5) | type);
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

interrupt_frame_t *isr_common_handler(interrupt_frame_t *frame)
{
	if (frame->vector == APIC_SPURIOUS_VECTOR)
		return frame;

	if (frame->vector == 0x80)
		return syscall_dispatch(frame);

	if (frame->vector == APIC_TIMER_VECTOR) {
		frame = sched_signal_deliver(frame);
		apic_send_eoi();
		return sched_tick(frame);
	}

	if (frame->vector < IRQ_BASE) {
		if ((frame->cs & 3) == 3) {
			tcb_t *thread = sched_current();
			pcb_t *process = thread ? thread->process : NULL;

			if (frame->vector == 14 && process && process->vas &&
				vas_handle_page_fault(process->vas, frame->cr2, frame->err) == 0) {
				return frame;
			}

			interrupt_frame_t *next = sched_handle_user_exception(
				frame, exception_signal(frame->vector),
				_exception_str[frame->vector]);
			if (next)
				return next;
		}

		kpanic(frame, "%s", _exception_str[frame->vector]);
	}

	interrupt_frame_t *next =
		irq_dispatch((uint8_t)(frame->vector - IRQ_BASE), frame);
	apic_send_eoi();
	return next;
}
