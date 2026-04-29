#ifndef _LYR_SYNC_SPINLOCK_H
#define _LYR_SYNC_SPINLOCK_H

#include <stdatomic.h>
#include <stdbool.h>

typedef struct {
	atomic_flag flag;
} spinlock_t;

#define SPINLOCK_INIT    \
	{                    \
		ATOMIC_FLAG_INIT \
	}

static inline void spinlock_acquire(spinlock_t *lock)
{
	while (atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire))
		__asm__ volatile("pause" ::: "memory");
}

static inline bool spinlock_try_acquire(spinlock_t *lock)
{
	return !atomic_flag_test_and_set_explicit(&lock->flag, memory_order_acquire);
}

static inline void spinlock_release(spinlock_t *lock)
{
	atomic_flag_clear_explicit(&lock->flag, memory_order_release);
}

#endif /* _LYR_SYNC_SPINLOCK_H */
