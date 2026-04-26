#include <mm/paging.h>
#include <mm/pmm.h>
#include <mm/pfndb.h>
#include <mm/page.h>
#include <lib/string.h>
#include <lib/align.h>
#include <debug/assert.h>
#include <debug/panic.h>

#define PAGE_FRAME_MASK 0x000FFFFFFFFFF000ULL
#define PML_IDX_MASK 0x1ffULL
#define PML_SHIFT_L1 12
#define PML_SHIFT_L2 21
#define PML_SHIFT_L3 30
#define PML_SHIFT_L4 39

static inline uint16_t _pml1_idx(uintptr_t v)
{
	return (v >> PML_SHIFT_L1) & PML_IDX_MASK;
}
static inline uint16_t _pml2_idx(uintptr_t v)
{
	return (v >> PML_SHIFT_L2) & PML_IDX_MASK;
}
static inline uint16_t _pml3_idx(uintptr_t v)
{
	return (v >> PML_SHIFT_L3) & PML_IDX_MASK;
}
static inline uint16_t _pml4_idx(uintptr_t v)
{
	return (v >> PML_SHIFT_L4) & PML_IDX_MASK;
}

static uint64_t _alloc_pt(void)
{
	page_t *page = palloc_page(); /* refcount=1, PAGE_USED */
	if (!page)
		return 0;

	uint64_t phys = pfndb_page_to_phys(page);
	memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
	return phys;
}

static void _free_pt(uint64_t phys)
{
	page_t *page = pfndb_phys_to_page(phys);
	if (!page)
		kpanic(NULL, "paging: _free_pt on untracked physical page 0x%llx",
			   phys);

	page_unref(page);
}

static uint64_t *_table_next(uint64_t *tbl, uint64_t idx, uint64_t create_flags)
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

static uint64_t *_leaf_entry(uint64_t *pml4, uint64_t virt,
							 uint64_t create_flags)
{
	uint64_t *pml3 = _table_next(pml4, _pml4_idx(virt), create_flags);
	if (!pml3)
		return NULL;

	uint64_t *pml2 = _table_next(pml3, _pml3_idx(virt), create_flags);
	if (!pml2)
		return NULL;

	uint64_t *pml1 = _table_next(pml2, _pml2_idx(virt), create_flags);
	if (!pml1)
		return NULL;

	return &pml1[_pml1_idx(virt)];
}

static void _ptable_free_level(uint64_t *tbl, int depth)
{
	if (depth == 1) {
		/* leaf: each present entry is a mapped user/kernel page
         * unshare it so the PMM knows one fewer mapping exists    */
		for (int i = 0; i < 512; i++) {
			uint64_t e = tbl[i];
			if (!(e & VMM_PRESENT))
				continue;

			uint64_t phys = e & PAGE_FRAME_MASK;
			page_t *page = pfndb_phys_to_page(phys);
			if (!page)
				continue; /* untracked region */

			page_unshare(page);
			page_unref(page);
		}

		_free_pt(VIRT_TO_PHYS((uint64_t)tbl));
		return;
	}

	for (int i = 0; i < 512; i++) {
		uint64_t e = tbl[i];
		if (!(e & VMM_PRESENT))
			continue;
		if (e & VMM_HUGE)
			continue; /* huge pages: callers responsibility, for now */

		uint64_t *child = (uint64_t *)PHYS_TO_VIRT(e & PAGE_FRAME_MASK);
		_ptable_free_level(child, depth - 1);
	}

	_free_pt(VIRT_TO_PHYS((uint64_t)tbl));
}

void map_page(ptable_t *pt, uint64_t virt, page_t *page, uint64_t flags)
{
	assert(pt != NULL);
	assert(page != NULL);

	virt = ALIGN_DOWN(virt, PAGE_SIZE);

	uint64_t phys = pfndb_page_to_phys(page);
	uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT((uint64_t)pt);
	uint64_t table_flags = VMM_PRESENT | VMM_WRITABLE;

	if (flags & VMM_USER)
		table_flags |= VMM_USER;

	uint64_t *pte = _leaf_entry(pml4, virt, table_flags);
	if (!pte)
		kpanic(NULL, "paging: failed to allocate page-table page");

	if (*pte & VMM_PRESENT)
		kpanic(NULL, "paging: map_page on already-present PTE (virt=0x%llx)",
			   virt);

	*pte = phys | (flags & ~PAGE_FRAME_MASK) | VMM_PRESENT;

	page_share(page); /* sharecount++, sets PAGE_SHARED if > 1 */
	page_ref(page); /* refcount++ — this mapping owns a reference */

	asm volatile("invlpg (%0)" ::"r"(virt) : "memory");
}

void map_page_phys(ptable_t *pt, uint64_t virt, uint64_t phys, uint64_t flags)
{
	assert(pt != NULL);

	virt = ALIGN_DOWN(virt, PAGE_SIZE);
	phys = ALIGN_DOWN(phys, PAGE_SIZE);

	uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT((uint64_t)pt);
	uint64_t table_flags = VMM_PRESENT | VMM_WRITABLE;

	if (flags & VMM_USER)
		table_flags |= VMM_USER;

	uint64_t *pte = _leaf_entry(pml4, virt, table_flags);
	if (!pte)
		kpanic(NULL, "paging: failed to allocate page-table page");

	if (*pte & VMM_PRESENT)
		kpanic(NULL,
			   "paging: map_page_phys on already-present PTE (virt=0x%llx)",
			   virt);

	*pte = phys | (flags & ~PAGE_FRAME_MASK) | VMM_PRESENT;

	asm volatile("invlpg (%0)" ::"r"(virt) : "memory");
}

void unmap_page(ptable_t *pt, uint64_t virt)
{
	assert(pt != NULL);

	virt = ALIGN_DOWN(virt, PAGE_SIZE);

	uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT((uint64_t)pt);
	uint64_t *pte = _leaf_entry(pml4, virt, 0);

	if (!pte || !(*pte & VMM_PRESENT))
		return;

	uint64_t phys = *pte & PAGE_FRAME_MASK;
	*pte = 0;
	asm volatile("invlpg (%0)" ::"r"(virt) : "memory");

	page_t *page = pfndb_phys_to_page(phys);
	if (!page)
		return; /* untracked region */

	page_unshare(page); /* sharecount--, clears PAGE_SHARED if ≤ 1 */
	page_unref(page); /* refcount--; frees page if it hits 0 */
}

uint64_t get_phys(ptable_t *pt, uint64_t virt)
{
	assert(pt != NULL);

	uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT((uint64_t)pt);
	uint64_t *pte = _leaf_entry(pml4, ALIGN_DOWN(virt, PAGE_SIZE), 0);

	if (!pte || !(*pte & VMM_PRESENT))
		return 0;

	return (*pte & PAGE_FRAME_MASK) + (virt & (PAGE_SIZE - 1));
}

ptable_t *ptable_create(void)
{
	page_t *page = palloc_page();
	if (!page)
		kpanic(NULL, "paging: failed to allocate PML4");

	uint64_t phys = pfndb_page_to_phys(page);
	memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
	return (ptable_t *)phys;
}

void ptable_destroy(ptable_t *pt)
{
	assert(pt != NULL);
	uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT((uint64_t)pt);
	_ptable_free_level(pml4, 4);
}