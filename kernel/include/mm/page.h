#ifndef _LYR_MM_PAGE_H
#define _LYR_MM_PAGE_H

#include <stdint.h>

#define PAGE_SIZE 0x1000

#define PAGE_FREE (1u << 0)
#define PAGE_USED (1u << 1)
#define PAGE_RESERVED (1u << 2)

/* page metadata */
typedef struct {
	uint8_t flags;
	uint32_t refcount;
} __attribute__((aligned(64))) page_t;

extern uint64_t _lyr_hhdm_offset;

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + _lyr_hhdm_offset))
#define VIRT_TO_PHYS(v) ((uint64_t)(v) - _lyr_hhdm_offset)

#endif