#ifndef _LYR_MM_PAGING_H
#define _LYR_MM_PAGING_H

#include <stdint.h>

#define VMM_PRESENT       0b1
#define VMM_WRITABLE      0b10
#define VMM_USER          0b100
#define VMM_WRITETHROUGH  0b1000
#define VMM_CACHE_DISABLE 0b10000
#define VMM_COW           0b1000000000

#define VMM_NX            0b1ull << 63

typedef struct {
    uint64_t entries[512];
} ptable_t;

void map_page(ptable_t* pt, uint64_t virt, uint64_t phys, uint64_t flags);
void unmap_page(ptable_t* pt, uint64_t virt);

#endif // _LYR_MM_PAGING_H