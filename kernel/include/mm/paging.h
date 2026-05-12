#ifndef _LYR_MM_PAGING_H
#define _LYR_MM_PAGING_H

#include <stdint.h>
#include <mm/page.h>

typedef uint64_t ptable_t;

#define VMM_PRESENT (1ULL << 0)
#define VMM_WRITABLE (1ULL << 1)
#define VMM_USER (1ULL << 2)
#define VMM_PWT (1ULL << 3)
#define VMM_PCD (1ULL << 4)
#define VMM_ACCESSED (1ULL << 5)
#define VMM_DIRTY (1ULL << 6)
#define VMM_HUGE (1ULL << 7)
#define VMM_GLOBAL (1ULL << 8)
#define VMM_NX (1ULL << 63)

#define VMM_FLAGS_KERNEL_RW (VMM_PRESENT | VMM_WRITABLE)
#define VMM_FLAGS_KERNEL_RO (VMM_PRESENT)
#define VMM_FLAGS_USER_RW (VMM_PRESENT | VMM_WRITABLE | VMM_USER)
#define VMM_FLAGS_USER_RO (VMM_PRESENT | VMM_USER)
#define VMM_FLAGS_MMIO (VMM_PRESENT | VMM_WRITABLE | VMM_PCD | VMM_PWT | VMM_NX)

void map_page(ptable_t *pt, uint64_t virt, page_t *page, uint64_t flags);
void map_page_phys(ptable_t *pt, uint64_t virt, uint64_t phys, uint64_t flags);
void map_mmio(ptable_t *pt, uint64_t virt, uint64_t phys, uint64_t npages);
void unmap_page(ptable_t *pt, uint64_t virt);

uint64_t get_phys(ptable_t *pt, uint64_t virt);
uint64_t get_mapping_flags(ptable_t *pt, uint64_t virt);
ptable_t *ptable_create(void);
void ptable_destroy(ptable_t *pt);
void ptable_free_empty(ptable_t *pt, uint64_t virt);

void paging_init(void);

extern ptable_t *kernel_ptable;
extern uint64_t _lyr_kstack_top;
extern uint64_t _lyr_kvirt;
extern uint64_t _lyr_kphys;

#endif // _LYR_MM_PAGING_H
