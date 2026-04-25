#include <mm/paging.h>
#include <mm/pmm.h>
#include <lib/string.h>
#include <mm/page.h>
#include <lib/align.h>
#include <debug/assert.h>

/* helpers */
#define PAGE_FRAME_MASK 0x000FFFFFFFFFF000ULL
#define PML_IDX_MASK 0x1ffULL
#define PML_SHIFT_L1 12
#define PML_SHIFT_L2 21
#define PML_SHIFT_L3 30
#define PML_SHIFT_L4 39

static inline uint16_t _pml1_index(uintptr_t v)
{
	return (v >> PML_SHIFT_L1) & PML_IDX_MASK;
}

static inline uint16_t _pml2_index(uintptr_t v)
{
	return (v >> PML_SHIFT_L2) & PML_IDX_MASK;
}

static inline uint16_t _pml3_index(uintptr_t v)
{
	return (v >> PML_SHIFT_L3) & PML_IDX_MASK;
}

static inline uint16_t _pml4_index(uintptr_t v)
{
	return (v >> PML_SHIFT_L4) & PML_IDX_MASK;
}

static inline uint64_t _alloc_pt(void)
{
	uint64_t phys = (uint64_t)palloc_single();
	if (!phys)
		return 0;
	memset((void *)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
	return phys;
}

static inline uint64_t *_table_next(uint64_t *tbl, uint64_t idx,
									uint64_t create_flags)
{
	uint64_t e = tbl[idx];

	if (e & VMM_PRESENT)
		return (uint64_t *)PHYS_TO_VIRT(e & PAGE_FRAME_MASK);

	if (!create_flags)
		return NULL;

	uint64_t phys = _alloc_pt();
	if (!phys)
		return NULL;

	tbl[idx] = phys | create_flags;
	return (uint64_t *)PHYS_TO_VIRT(phys);
}

static inline uint64_t *_leaf_entry(uint64_t *pml4, uint64_t virt,
									uint64_t create_flags)
{
	uint64_t *pml3 = _table_next(pml4, _pml4_index(virt), create_flags);
	if (!pml3)
		return NULL;

	uint64_t *pml2 = _table_next(pml3, _pml3_index(virt), create_flags);
	if (!pml2)
		return NULL;

	uint64_t *pml1 = _table_next(pml2, _pml2_index(virt), create_flags);
	if (!pml1)
		return NULL;

	return &pml1[_pml1_index(virt)];
}

/* main lyr API */
void map_page(ptable_t *pt, uint64_t virt, uint64_t phys, uint64_t flags)
{
	assert(pt != NULL);

	virt = ALIGN_DOWN(virt, PAGE_SIZE);
	phys = ALIGN_DOWN(phys, PAGE_SIZE);

	uint64_t *pml4 = (uint64_t *)pt;
	uint64_t table_flags = VMM_PRESENT | VMM_WRITABLE;

	if (flags & VMM_USER)
		table_flags |= VMM_USER;

	uint64_t *pte = _leaf_entry(pml4, virt, table_flags);
	assert(pte != NULL);

	*pte = phys | flags | VMM_PRESENT;

	asm volatile("invlpg (%0)" ::"r"(virt) : "memory");
}

void unmap_page(ptable_t *pt, uint64_t virt)
{
	assert(pt != NULL);

	virt = ALIGN_DOWN(virt, PAGE_SIZE);

	uint64_t *pml4 = (uint64_t *)pt;
	uint64_t *pml3 = _table_next(pml4, _pml4_index(virt), 0);
	if (!pml3)
		return;

	uint64_t *pml2 = _table_next(pml3, _pml3_index(virt), 0);
	if (!pml2)
		return;

	uint64_t *pml1 = _table_next(pml2, _pml2_index(virt), 0);
	if (!pml1)
		return;

	uint64_t *pte = &pml1[_pml1_index(virt)];
	if (!(*pte & VMM_PRESENT))
		return;

	*pte = 0;

	asm volatile("invlpg (%0)" ::"r"(virt) : "memory");
}