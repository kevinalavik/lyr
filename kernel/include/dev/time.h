#ifndef _LYR_DEV_TIME_H
#define _LYR_DEV_TIME_H

#include <stdint.h>

#define LYR_CLOCK_REALTIME 0
#define LYR_CLOCK_MONOTONIC 1
#define LYR_CLOCK_MONOTONIC_RAW 4

#define NSEC_PER_SEC 1000000000LL
#define USEC_PER_SEC 1000000LL
#define MSEC_PER_SEC 1000LL

#define NSEC_PER_MSEC 1000000ULL

typedef struct time_source {
	const char *name;
	int (*read_wall)(int64_t *sec, long *nsec, void *ctx);
	void *ctx;
} time_source_t;

typedef struct time_timeout {
	int finite;
	uint64_t deadline_ns;
} time_timeout_t;

int time_init(void);
int time_register_source(const time_source_t *source);
int time_get(int clock_id, int64_t *sec, long *nsec);
uint64_t time_monotonic_ns(void);
uint64_t time_ms_to_ns(uint64_t ms);
int time_timeout_after_ms(int timeout_ms, time_timeout_t *out);
int time_timeout_after_timespec(int64_t sec, long nsec, time_timeout_t *out);
int time_timeout_expired(const time_timeout_t *timeout);
void time_sleep_until_interrupt_or_timeout(const time_timeout_t *timeout);

#endif /* _LYR_DEV_TIME_H */
