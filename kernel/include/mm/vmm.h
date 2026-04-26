#ifndef _LYR_MM_VMM_H
#define _LYR_MM_VMM_H

#include <stdint.h>
#include <stddef.h>
#include <mm/page.h>
#include <mm/paging.h>

#define VAD_PROT_MASK 0x0000000000000FFFull
#define VAD_ANONYMOUS (1ULL << 12) /* backed by zero-filled PMM pages */
#define VAD_FIXED (1ULL << 13) /* address was caller-specified     */
#define VAD_MAPPED (1ULL << 14) /* pages already faulted/committed  */

#define VAS_USER_START 0x0000000000001000ull
#define VAS_USER_END 0x00007FFFFFFFFFFFull

typedef struct vad {
	uint64_t start;
	uint64_t end;
	uint64_t flags;
	struct vad *next;
} vad_t;

typedef struct vas {
	ptable_t *pml4;
	vad_t *list_head;
	uint64_t user_start;
} vas_t;

vas_t *vas_create(ptable_t *pt);
void vas_destroy(vas_t *vas);

uint64_t vas_map_anon(vas_t *vas, uint64_t hint, size_t length, uint64_t flags);
uint64_t vas_map_phys(vas_t *vas, uint64_t hint, uint64_t phys, size_t length,
					  uint64_t flags);
int vas_unmap(vas_t *vas, uint64_t start, size_t length);
int vas_protect(vas_t *vas, uint64_t start, size_t length, uint64_t new_prot);
vad_t *vas_find(vas_t *vas, uint64_t addr);

void vas_switch(vas_t *vas);
vas_t *vas_adopt(ptable_t *existing_pml4);

#endif /* _LYR_MM_VMM_H */