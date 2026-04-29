#include <debug/panic.h>
#include <debug/log.h>
#include <util/kprintf.h>
#include <cpu/instr.h>
#include <stdatomic.h>

static atomic_flag panic_lock = ATOMIC_FLAG_INIT;

static void dump_registers(const interrupt_frame_t *f)
{
	log_err("panic", "---- CPU STATE ----");
	log_err("panic", "RIP=%016lx  CS =%016lx  RFLAGS=%016lx", f->rip, f->cs,
			f->rflags);
	log_err("panic", "RSP=%016lx  SS =%016lx", f->rsp, f->ss);
	log_err("panic", "RAX=%016lx  RBX=%016lx  RCX=%016lx  RDX=%016lx", f->rax,
			f->rbx, f->rcx, f->rdx);
	log_err("panic", "RSI=%016lx  RDI=%016lx  RBP=%016lx", f->rsi, f->rdi,
			f->rbp);
	log_err("panic", "R8 =%016lx  R9 =%016lx  R10=%016lx  R11=%016lx", f->r8,
			f->r9, f->r10, f->r11);
	log_err("panic", "R12=%016lx  R13=%016lx  R14=%016lx  R15=%016lx", f->r12,
			f->r13, f->r14, f->r15);
	log_err("panic", "CR0=%016lx  CR2=%016lx  CR3=%016lx  CR4=%016lx", f->cr0,
			f->cr2, f->cr3, f->cr4);
	log_err("panic", "ES =%016lx  DS =%016lx", f->es, f->ds);
	log_err("panic", "VEC=%016lu  ERR=%016lx", f->vector, f->err);
}

static void decode_page_fault(uint64_t err)
{
	log_err("panic", "Page Fault details:");
	log_err("panic", "  Present:  %s", (err & 1) ? "yes" : "no (not-present)");
	log_err("panic", "  Write:    %s", (err & 2) ? "write" : "read");
	log_err("panic", "  User:     %s", (err & 4) ? "user mode" : "kernel mode");
	log_err("panic", "  Reserved: %s", (err & 8) ? "reserved bit set" : "no");
	log_err("panic", "  Exec:     %s", (err & 16) ? "instruction fetch" : "no");
}

__attribute__((noreturn)) void kpanic(interrupt_frame_t *frame, const char *fmt,
									  ...)
{
	__asm__ volatile("cli");
	while (atomic_flag_test_and_set_explicit(&panic_lock, memory_order_acquire))
		__asm__ volatile("pause" ::: "memory");

	log_err("panic", "========= KERNEL PANIC =========");

	if (fmt) {
		char buf[512];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		log_err("panic", "reason: %s", buf);
	} else {
		log_err("panic", "reason: (no message)");
	}

	log_err("panic", "");

	if (frame) {
		if (frame->vector == 14) {
			log_err("panic", "CR2 (fault address): 0x%lx", frame->cr2);
			decode_page_fault(frame->err);
		}
		dump_registers(frame);
	}

	log_err("panic", "System halted.");
	log_err("panic", "================================");

	for (;;)
		__asm__ volatile("hlt");
}