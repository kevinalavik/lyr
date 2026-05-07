#ifndef _LYR_DEV_TIME_H
#define _LYR_DEV_TIME_H

#include <stdint.h>

#define LYR_CLOCK_REALTIME 0
#define LYR_CLOCK_MONOTONIC 1
#define LYR_CLOCK_MONOTONIC_RAW 4

#define NSEC_PER_SEC 1000000000LL
#define USEC_PER_SEC 1000000LL
#define MSEC_PER_SEC 1000LL

typedef struct time_source {
	const char *name;
	int (*read_wall)(int64_t *sec, long *nsec, void *ctx);
	void *ctx;
} time_source_t;

int time_init(void);
int time_register_source(const time_source_t *source);
int time_get(int clock_id, int64_t *sec, long *nsec);
uint64_t time_monotonic_ns(void);

#endif /* _LYR_DEV_TIME_H */
