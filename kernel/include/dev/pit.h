#ifndef _LYR_DEV_PIT_H
#define _LYR_DEV_PIT_H

#include <stdint.h>

#define PIT_CLOCK 1193182
#define PIT_COMMAND 0x43
#define PIT_COUNTER0 0x40

void pit_init(uint16_t freq);

void pit_reload(void);
void pit_set_freq(uint16_t freq);
void pit_set_div(uint16_t div);

int pit_is_initialized(void);
uint16_t pit_get_hz(void);
uint64_t pit_get_ticks(void);

#endif // _LYR_DEV_PIT_H