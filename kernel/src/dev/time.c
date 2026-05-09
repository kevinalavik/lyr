#include <dev/time.h>
#include <dev/pit.h>
#include <errno.h>
#include <debug/log.h>
#include <lib/string.h>
#include <sync/spinlock.h>
#include <cpu/instr.h>

static spinlock_t time_lock = SPINLOCK_INIT;
static time_source_t wall_source;
static int wall_source_registered;
static int64_t wall_base_sec;
static long wall_base_nsec;
static uint64_t wall_base_mono_ns;
static uint64_t monotonic_base_ticks;
static uint16_t monotonic_base_hz;
static int time_ready;

int time_init(void)
{
	spinlock_acquire(&time_lock);
	memset(&wall_source, 0, sizeof(wall_source));
	wall_source_registered = 0;
	wall_base_sec = 0;
	wall_base_nsec = 0;
	wall_base_mono_ns = 0;
	monotonic_base_ticks = pit_get_ticks();
	monotonic_base_hz = pit_get_hz();
	time_ready = 1;
	spinlock_release(&time_lock);

	log_info("time", "time manager ok");
	return 0;
}

uint64_t time_monotonic_ns(void)
{
	uint16_t hz = pit_get_hz();
	if (!hz)
		hz = monotonic_base_hz;
	if (!hz)
		return 0;

	uint64_t ticks = pit_get_ticks();
	uint64_t base = monotonic_base_ticks;
	if (ticks < base)
		base = 0;
	return ((ticks - base) * (uint64_t)NSEC_PER_SEC) / hz;
}

uint64_t time_ms_to_ns(uint64_t ms)
{
	/*
	 * Convert milliseconds to nanoseconds.
	 *
	 * The old overflow check divided by MSEC_PER_SEC and NSEC_PER_MSEC,
	 * which is not the conversion being performed. 1 ms is exactly
	 * 1,000,000 ns.
	 */
	if (ms > UINT64_MAX / (uint64_t)NSEC_PER_MSEC)
		return UINT64_MAX;

	return ms * (uint64_t)NSEC_PER_MSEC;
}

static uint64_t time_add_ns_saturating(uint64_t a, uint64_t b)
{
	if (UINT64_MAX - a < b)
		return UINT64_MAX;

	return a + b;
}

int time_timeout_after_ms(int timeout_ms, time_timeout_t *out)
{
	if (!out)
		return -EINVAL;

	if (timeout_ms < 0) {
		out->finite = 0;
		out->deadline_ns = 0;
		return 0;
	}

	out->finite = 1;
	out->deadline_ns = time_add_ns_saturating(
		time_monotonic_ns(), time_ms_to_ns((uint64_t)timeout_ms));
	return 0;
}

int time_timeout_after_timespec(int64_t sec, long nsec, time_timeout_t *out)
{
	if (!out)
		return -EINVAL;

	if (sec < 0 || nsec < 0 || nsec >= NSEC_PER_SEC)
		return -EINVAL;

	uint64_t sec_ns;
	if ((uint64_t)sec > UINT64_MAX / (uint64_t)NSEC_PER_SEC)
		sec_ns = UINT64_MAX;
	else
		sec_ns = (uint64_t)sec * (uint64_t)NSEC_PER_SEC;

	uint64_t duration_ns = time_add_ns_saturating(sec_ns, (uint64_t)nsec);

	out->finite = 1;
	out->deadline_ns = time_add_ns_saturating(time_monotonic_ns(), duration_ns);
	return 0;
}

int time_timeout_expired(const time_timeout_t *timeout)
{
	if (!timeout || !timeout->finite)
		return 0;

	return (int64_t)(time_monotonic_ns() - timeout->deadline_ns) >= 0;
}

void time_sleep_until_interrupt_or_timeout(const time_timeout_t *timeout)
{
	if (time_timeout_expired(timeout))
		return;

	__asm__ volatile("sti; hlt; cli" ::: "memory");
}

int time_register_source(const time_source_t *source)
{
	if (!source || !source->name || !source->read_wall)
		return -EINVAL;

	int64_t sec;
	long nsec;
	int r = source->read_wall(&sec, &nsec, source->ctx);
	if (r != 0)
		return r;
	if (nsec < 0 || nsec >= NSEC_PER_SEC)
		return -EINVAL;

	spinlock_acquire(&time_lock);
	wall_source = *source;
	wall_base_sec = sec;
	wall_base_nsec = nsec;
	wall_base_mono_ns = time_monotonic_ns();
	wall_source_registered = 1;
	spinlock_release(&time_lock);

	log_info("time", "registered wall clock source %s", source->name);
	return 0;
}

int time_get(int clock_id, int64_t *sec, long *nsec)
{
	if (!sec || !nsec)
		return -EINVAL;
	if (!time_ready)
		return -ENODEV;

	switch (clock_id) {
	case LYR_CLOCK_REALTIME: {
		int64_t base_sec;
		long base_nsec;
		uint64_t base_mono_ns;

		spinlock_acquire(&time_lock);
		if (!wall_source_registered) {
			spinlock_release(&time_lock);
			return -ENODEV;
		}
		base_sec = wall_base_sec;
		base_nsec = wall_base_nsec;
		base_mono_ns = wall_base_mono_ns;
		spinlock_release(&time_lock);

		uint64_t now_ns = time_monotonic_ns();
		uint64_t elapsed_ns =
			now_ns >= base_mono_ns ? now_ns - base_mono_ns : 0;
		int64_t out_sec = base_sec + (int64_t)(elapsed_ns / NSEC_PER_SEC);
		long out_nsec = base_nsec + (long)(elapsed_ns % NSEC_PER_SEC);
		if (out_nsec >= NSEC_PER_SEC) {
			out_sec++;
			out_nsec -= NSEC_PER_SEC;
		}
		*sec = out_sec;
		*nsec = out_nsec;
		return 0;
	}
	case LYR_CLOCK_MONOTONIC:
	case LYR_CLOCK_MONOTONIC_RAW: {
		uint64_t ns = time_monotonic_ns();
		*sec = (int64_t)(ns / (uint64_t)NSEC_PER_SEC);
		*nsec = (long)(ns % (uint64_t)NSEC_PER_SEC);
		return 0;
	}
	default:
		return -EINVAL;
	}
}