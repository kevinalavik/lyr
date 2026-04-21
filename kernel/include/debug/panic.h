#ifndef _LYR_DEBUG_PANIC_H
#define _LYR_DEBUG_PANIC_H

#include <cpu/idt.h>

__attribute__((noreturn))
void kpanic(interrupt_frame_t *frame, const char *fmt, ...);

#endif // _LYR_DEBUG_PANIC_H