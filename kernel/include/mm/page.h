#ifndef _LYR_MM_PAGE_H
#define _LYR_MM_PAGE_H

#include <stdint.h>

#define PAGE_SIZE 0x1000

// page  flags
#define PAGE_FREE (1u << 0) /* on freelist, not owned */
#define PAGE_USED (1u << 1) /* owned by at least one mapping */
#define PAGE_RESERVED (1u << 2) /* never freed */
#define PAGE_SHARED \
	(1u << 3) /* mapped in more than one place (sharecount > 1) */
#define PAGE_COW (1u << 4) /* copy-on-write */
#define PAGE_POISON (1u << 5) /* use-after-free detection, never re-alloc */

/* any "allocated" state */
#define PAGE_ALLOCATED (PAGE_USED | PAGE_RESERVED)

/* page metadata */
typedef struct page page_t;

typedef struct page {
	union {
		page_t *next;
		uint64_t *pte;
	} u1;

	union {
		page_t *prev;
		uint64_t sharecount;
	} u2;

	uint64_t flags;
	uint32_t refcount;
} __attribute__((aligned(64))) page_t;

extern uint64_t _lyr_hhdm_offset;

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + _lyr_hhdm_offset))
#define VIRT_TO_PHYS(v) ((uint64_t)(v) - _lyr_hhdm_offset)

#define page_is_free(p) ((p)->flags & PAGE_FREE)
#define page_is_used(p) ((p)->flags & PAGE_USED)
#define page_is_reserved(p) ((p)->flags & PAGE_RESERVED)
#define page_is_shared(p) ((p)->flags & PAGE_SHARED)
#define page_is_cow(p) ((p)->flags & PAGE_COW)
#define page_is_poisoned(p) ((p)->flags & PAGE_POISON)

#endif