#include <mm/paging.h>
#include <mm/pmm.h>
#include <mm/pfndb.h>
#include <mm/page.h>
#include <lib/string.h>
#include <lib/align.h>
#include <debug/assert.h>
#include <debug/panic.h>
#include <mm/heap.h>
#include <sync/spinlock.h>

ptable_t *kernel_ptable = NULL;
extern char __limine_requests_start[];
extern char __limine_requests_end[];
extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];

#define PAGE_FRAME_MASK 0x000FFFFFFFFFF000ULL
#define PML_IDX_MASK 0x1ffULL
#define PML_SHIFT_L1 12
#define PML_SHIFT_L2 21
#define PML_SHIFT_L3 30
#define PML_SHIFT_L4 39

static spinlock_t paging_lock = SPINLOCK_INIT;

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

static int _table_empty(uint64_t *tbl)
{
	for (int i = 0; i < 512; i++)
		if (tbl[i] & VMM_PRESENT)
			return 0;
	return 1;
}

static void ptable_free_empty_locked(ptable_t *pt, uint64_t virt);

void map_page(ptable_t *pt, uint64_t virt, page_t *page, uint64_t flags)
{
	assert(pt != NULL);
	assert(page != NULL);

	spinlock_acquire(&paging_lock);
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
	spinlock_release(&paging_lock);
}

void map_page_phys(ptable_t *pt, uint64_t virt, uint64_t phys, uint64_t flags)
{
	assert(pt != NULL);

	spinlock_acquire(&paging_lock);
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
		goto out;

	*pte = phys | (flags & ~PAGE_FRAME_MASK) | VMM_PRESENT;

	asm volatile("invlpg (%0)" ::"r"(virt) : "memory");
out:
	spinlock_release(&paging_lock);
}

void unmap_page(ptable_t *pt, uint64_t virt)
{
	assert(pt != NULL);

	spinlock_acquire(&paging_lock);
	virt = ALIGN_DOWN(virt, PAGE_SIZE);

	uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT((uint64_t)pt);
	uint64_t *pte = _leaf_entry(pml4, virt, 0);

	if (!pte || !(*pte & VMM_PRESENT)) {
		spinlock_release(&paging_lock);
		return;
	}

	uint64_t phys = *pte & PAGE_FRAME_MASK;
	*pte = 0;
	asm volatile("invlpg (%0)" ::"r"(virt) : "memory");

	page_t *page = pfndb_phys_to_page(phys);
	if (page) {
		page_unshare(page);
		page_unref(page);
	}

	ptable_free_empty_locked(pt, virt);
	spinlock_release(&paging_lock);
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

uint64_t get_mapping_flags(ptable_t *pt, uint64_t virt)
{
	assert(pt != NULL);

	uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT((uint64_t)pt);
	uint64_t *pte = _leaf_entry(pml4, ALIGN_DOWN(virt, PAGE_SIZE), 0);

	if (!pte || !(*pte & VMM_PRESENT))
		return 0;

	return *pte & ~PAGE_FRAME_MASK;
}

ptable_t *ptable_create(void)
{
	page_t *page = palloc_page();
	if (!page)
		kpanic(NULL, "paging: failed to allocate PML4");

	uint64_t phys = pfndb_page_to_phys(page);
	uint64_t *pml4 = PHYS_TO_VIRT(phys);
	memset(pml4, 0, PAGE_SIZE);
	if (kernel_ptable) {
		uint64_t *kpml4 = PHYS_TO_VIRT((uint64_t)kernel_ptable);
		for (int i = 256; i < 512; i++)
			pml4[i] = kpml4[i];
	}
	return (ptable_t *)phys;
}

void ptable_destroy(ptable_t *pt)
{
	assert(pt != NULL);
	uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT((uint64_t)pt);

	if (pt == kernel_ptable) {
		_ptable_free_level(pml4, 4);
		return;
	}

	for (int i = 0; i < 256; i++) {
		uint64_t e = pml4[i];
		if (!(e & VMM_PRESENT))
			continue;
		if (e & VMM_HUGE)
			continue;

		uint64_t *child = (uint64_t *)PHYS_TO_VIRT(e & PAGE_FRAME_MASK);
		_ptable_free_level(child, 3);
	}

	_free_pt((uint64_t)pt);
}

static void ptable_free_empty_locked(ptable_t *pt, uint64_t virt)
{
	uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT((uint64_t)pt);

	uint64_t i4 = _pml4_idx(virt);
	if (!(pml4[i4] & VMM_PRESENT))
		return;
	uint64_t *pml3 = (uint64_t *)PHYS_TO_VIRT(pml4[i4] & PAGE_FRAME_MASK);

	uint64_t i3 = _pml3_idx(virt);
	if (!(pml3[i3] & VMM_PRESENT))
		return;
	uint64_t *pml2 = (uint64_t *)PHYS_TO_VIRT(pml3[i3] & PAGE_FRAME_MASK);

	uint64_t i2 = _pml2_idx(virt);
	if (!(pml2[i2] & VMM_PRESENT))
		return;
	uint64_t *pml1 = (uint64_t *)PHYS_TO_VIRT(pml2[i2] & PAGE_FRAME_MASK);

	if (_table_empty(pml1)) {
		_free_pt(pml2[i2] & PAGE_FRAME_MASK);
		pml2[i2] = 0;
	} else
		return;

	if (_table_empty(pml2)) {
		_free_pt(pml3[i3] & PAGE_FRAME_MASK);
		pml3[i3] = 0;
	} else
		return;

	if (_table_empty(pml3)) {
		_free_pt(pml4[i4] & PAGE_FRAME_MASK);
		pml4[i4] = 0;
	}
}

void ptable_free_empty(ptable_t *pt, uint64_t virt)
{
	spinlock_acquire(&paging_lock);
	ptable_free_empty_locked(pt, virt);
	spinlock_release(&paging_lock);
}

void map_mmio(ptable_t *pt, uint64_t virt, uint64_t phys, uint64_t npages)
{
	assert(pt != NULL);

	virt = ALIGN_DOWN(virt, PAGE_SIZE);
	phys = ALIGN_DOWN(phys, PAGE_SIZE);

	uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT((uint64_t)pt);

	for (uint64_t i = 0; i < npages; i++) {
		uint64_t v = virt + i * PAGE_SIZE;
		uint64_t p = phys + i * PAGE_SIZE;

		uint64_t *pte = _leaf_entry(pml4, v, VMM_PRESENT | VMM_WRITABLE);
		if (!pte)
			kpanic(NULL, "paging: map_mmio failed to allocate pte for 0x%llx",
				   v);

		*pte = p | VMM_FLAGS_MMIO;

		asm volatile("invlpg (%0)" ::"r"(v) : "memory");
	}
}

void paging_init(void)
{
	kernel_ptable = ptable_create();
	assert(kernel_ptable);

#define MAP_SECTION(vstart, vend, flags)                         \
	do {                                                         \
		uint64_t _v = ALIGN_DOWN((uint64_t)(vstart), PAGE_SIZE); \
		uint64_t _e = ALIGN_UP((uint64_t)(vend), PAGE_SIZE);     \
		for (; _v < _e; _v += PAGE_SIZE) {                       \
			uint64_t _p = _v - _lyr_kvirt + _lyr_kphys;          \
			map_page_phys(kernel_ptable, _v, _p, (flags));       \
		}                                                        \
	} while (0)

	MAP_SECTION(__limine_requests_start, __limine_requests_end,
				VMM_PRESENT | VMM_NX);
	MAP_SECTION(__text_start, __text_end, VMM_PRESENT);
	MAP_SECTION(__rodata_start, __rodata_end, VMM_PRESENT | VMM_NX);
	MAP_SECTION(__data_start, __data_end, VMM_PRESENT | VMM_WRITABLE | VMM_NX);

#define KSTACK_SIZE (64 * 1024ULL)
	{
		uint64_t stack_top = ALIGN_UP((uint64_t)&_lyr_kstack_top, PAGE_SIZE);
		uint64_t stack_bottom = stack_top - KSTACK_SIZE;

		for (uint64_t v = stack_bottom; v < stack_top; v += PAGE_SIZE) {
			uint64_t p = v - _lyr_kvirt + _lyr_kphys;
			map_page_phys(kernel_ptable, v, p,
						  VMM_PRESENT | VMM_WRITABLE | VMM_NX);
		}
	}
#undef KSTACK_SIZE
	{
		uint64_t *new_pml4 = (uint64_t *)PHYS_TO_VIRT((uint64_t)kernel_ptable);
		uint64_t *boot_pml4 = (uint64_t *)PHYS_TO_VIRT(read_cr3());

		for (int i = 256; i < 512; i++)
			new_pml4[i] = boot_pml4[i];
	}

	log_debug("paging", "kernel sections mapped:");
	log_debug("paging", "  .limine_requests 0x%llx - 0x%llx (RO NX)",
			  (uint64_t)__limine_requests_start,
			  (uint64_t)__limine_requests_end);
	log_debug("paging", "  .text            0x%llx - 0x%llx (RX)",
			  (uint64_t)__text_start, (uint64_t)__text_end);
	log_debug("paging", "  .rodata          0x%llx - 0x%llx (RO NX)",
			  (uint64_t)__rodata_start, (uint64_t)__rodata_end);
	log_debug("paging", "  .data+.bss       0x%llx - 0x%llx (RW NX)",
			  (uint64_t)__data_start, (uint64_t)__data_end);
	log_debug("paging", "  stack            0x%llx - 0x%llx (RW NX)",
			  (uint64_t)&_lyr_kstack_top - 64 * 1024,
			  (uint64_t)&_lyr_kstack_top);

#undef MAP_SECTION
	write_cr3((uint64_t)kernel_ptable);
}
