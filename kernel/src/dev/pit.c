#include <dev/pit.h>
#include <cpu/instr.h>
#include <cpu/idt.h>
#include <stdint.h>
#include <stdbool.h>
#include <debug/log.h>
#include <sys/smp.h>

static bool _pit_init = false;
static volatile uint64_t _pit_ticks = 0;
static uint16_t _pit_hz = 0;

void tick(interrupt_frame_t *frame)
{
	(void)frame;
	_pit_ticks++;
	log_trace("pit", "ticking on CPU %d!", get_cpu_local()->cpu_index);
}

void pit_init(uint16_t freq)
{
	if (_pit_init) {
		log_warn("pit",
				 "tried initializing PIT, but PIT is already initialized");
		return;
	}

	cli();
	_pit_ticks = 0;
	_pit_hz = freq;

	uint16_t div = PIT_CLOCK / freq;

	outb(PIT_COMMAND, 0x36); // mode 3, rw
	outb(PIT_COUNTER0, div & 0xFF);
	outb(PIT_COUNTER0, div >> 8);
	irq_install(0, tick, NULL, 0xFF); /* install pit timer on all cores */

	_pit_init = true;
	log_trace("pit", "PIT is now running at %uHz (divisor = %u).",
			  PIT_CLOCK / div, div);
	sti();
}

int pit_is_initialized(void)
{
	return _pit_init ? 1 : 0;
}

uint16_t pit_get_hz(void)
{
	return _pit_hz;
}

uint64_t pit_get_ticks(void)
{
	return _pit_ticks;
}

void pit_reload(void)
{
	cli();
	uint16_t div = PIT_CLOCK / _pit_hz;
	outb(PIT_COMMAND, 0x36);
	outb(PIT_COUNTER0, div & 0xFF);
	outb(PIT_COUNTER0, div >> 8);
	sti();
}

void pit_set_freq(uint16_t freq)
{
	if (freq == 0)
		return;
	_pit_hz = freq;
	_pit_ticks = 0;
	pit_reload();
	log_trace("pit", "PIT frequency set to %uHz", freq);
}

void pit_set_div(uint16_t div)
{
	if (div == 0)
		return;
	_pit_hz = PIT_CLOCK / div;
	_pit_ticks = 0;
	pit_reload();
	log_debug("pit", "PIT divisor set to %u (%uHz)", div, _pit_hz);
}