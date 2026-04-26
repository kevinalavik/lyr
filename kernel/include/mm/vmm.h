#ifndef _LYR_MM_VMM_H
#define _LYR_MM_VMM_H

#include <stdint.h>
#include <mm/page.h>
#include <mm/paging.h>

typedef struct vad {
	uint64_t start;
	uint64_t end;
	uint64_t flags;
	struct vad *next;
} vad_t;

typedef struct vas {
	ptable_t *pml4;
	vad_t *list_head;
} vas_t;

#endif // _LYR_MM_VMM_H