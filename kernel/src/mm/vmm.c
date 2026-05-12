#include <mm/vmm.h>
#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/pfndb.h>
#include <mm/paging.h>
#include <lib/string.h>
#include <lib/align.h>
#include <debug/assert.h>
#include <debug/panic.h>
#include <fs/vfs.h>

static vad_t *_vad_alloc(uint64_t start, uint64_t end, uint64_t flags)
{
	vad_t *v = kzalloc(sizeof(vad_t));
	if (!v)
		return NULL;
	v->start = start;
	v->end = end;
	v->flags = flags;
	v->file = NULL;
	v->file_offset = 0;
	v->next = NULL;
	return v;
}

static vad_t *_vad_alloc_file(uint64_t start, uint64_t end, uint64_t flags,
							  struct vfs_node *file, uint64_t file_offset)
{
	vad_t *v = _vad_alloc(start, end, flags | VAD_FILE);
	if (!v)
		return NULL;
	v->file = file;
	v->file_offset = file_offset;
	vfs_node_ref(file);
	return v;
}

static void _vad_free(vad_t *v)
{
	if (!v)
		return;
	if (v->file)
		vfs_node_release(v->file);
	kfree(v);
}

static vad_t *_vad_clone_segment(vad_t *src, uint64_t start, uint64_t end)
{
	if (src->flags & VAD_FILE) {
		return _vad_alloc_file(start, end, src->flags, src->file,
							   src->file_offset + (start - src->start));
	}
	return _vad_alloc(start, end, src->flags);
}

static void _vad_insert(vas_t *vas, vad_t *v)
{
	vad_t **cur = &vas->list_head;
	while (*cur && (*cur)->start < v->start)
		cur = &(*cur)->next;
	v->next = *cur;
	*cur = v;
}

static void _vad_remove_range(vas_t *vas, uint64_t start, uint64_t end)
{
	vad_t **cur = &vas->list_head;
	while (*cur) {
		vad_t *v = *cur;
		if (v->end <= start || v->start >= end) {
			cur = &v->next;
			continue;
		}
		if (v->start < start) {
			vad_t *left = _vad_clone_segment(v, v->start, start);
			if (left) {
				left->next = v->next;
				*cur = left;
				cur = &left->next;
			}
		}
		if (v->end > end) {
			vad_t *right = _vad_clone_segment(v, end, v->end);
			if (right) {
				right->next = v->next;
				v->next = right;
			}
		}
		*cur = v->next;
		_vad_free(v);
	}
}

static uint64_t _vas_find_free(vas_t *vas, uint64_t hint, size_t length)
{
	uint64_t base = ALIGN_UP(hint, PAGE_SIZE);
	uint64_t end = base + length;

	for (vad_t *v = vas->list_head; v; v = v->next) {
		if (end <= v->start)
			break;
		if (base < v->end) {
			base = ALIGN_UP(v->end, PAGE_SIZE);
			end = base + length;
		}
	}

	if (end > VAS_USER_END || end < base)
		return 0;

	return base;
}

static int _vas_overlaps(vas_t *vas, uint64_t start, uint64_t end)
{
	for (vad_t *v = vas->list_head; v; v = v->next) {
		if (v->start >= end)
			break;
		if (v->end > start)
			return 1;
	}
	return 0;
}

static int _commit_anon(vas_t *vas, vad_t *vad)
{
	uint64_t prot = vad->flags & VAD_PROT_MASK;

	for (uint64_t va = vad->start; va < vad->end; va += PAGE_SIZE) {
		page_t *page = palloc_page();
		if (!page)
			return -1;

		map_page(vas->pml4, va, page, prot);
		page_unref(page);
	}
	return 0;
}

static int _commit_file(vas_t *vas, vad_t *vad)
{
	uint64_t prot = vad->flags & VAD_PROT_MASK;
	uint64_t file_off = vad->file_offset;

	for (uint64_t va = vad->start; va < vad->end; va += PAGE_SIZE) {
		page_t *page = NULL;
		int r = vfs_node_get_page(vad->file, file_off / PAGE_SIZE,
								  (prot & VMM_WRITABLE) != 0, &page);
		if (r != 0 || !page)
			return -1;

		map_page(vas->pml4, va, page, prot);
		file_off += PAGE_SIZE;
	}
	return 0;
}

static void _uncommit_range(vas_t *vas, uint64_t start, uint64_t end)
{
	for (uint64_t va = start; va < end; va += PAGE_SIZE)
		unmap_page(vas->pml4, va);
}

vas_t *vas_create(ptable_t *pt)
{
	vas_t *vas = kzalloc(sizeof(vas_t));
	if (!vas)
		return NULL;

	vas->pml4 = pt ? pt : ptable_create();
	vas->list_head = NULL;
	vas->user_start = VAS_USER_START;

	if (!vas->pml4) {
		kfree(vas);
		return NULL;
	}
	return vas;
}

void vas_destroy(vas_t *vas)
{
	if (!vas)
		return;

	vad_t *v = vas->list_head;
	while (v) {
		_uncommit_range(vas, v->start, v->end);
		vad_t *next = v->next;
		_vad_free(v);
		v = next;
	}

	ptable_destroy(vas->pml4);
	kfree(vas);
}

uint64_t vas_map_anon(vas_t *vas, uint64_t hint, size_t length, uint64_t flags)
{
	assert(vas);
	if (!length)
		return 0;

	length = ALIGN_UP(length, PAGE_SIZE);

	uint64_t base;
	if (flags & VAD_FIXED) {
		base = ALIGN_DOWN(hint, PAGE_SIZE);
		uint64_t end = base + length;
		if (end > VAS_USER_END || end < base)
			return 0;
		if (_vas_overlaps(vas, base, end))
			return 0;
	} else {
		uint64_t search_hint = hint ? hint : vas->user_start;
		base = _vas_find_free(vas, search_hint, length);
		if (!base)
			return 0;
	}

	vad_t *vad = _vad_alloc(base, base + length, flags | VAD_ANONYMOUS);
	if (!vad)
		return 0;

	if (_commit_anon(vas, vad) != 0) {
		_uncommit_range(vas, base, base + length);
		kfree(vad);
		return 0;
	}

	_vad_insert(vas, vad);
	vas->user_start = base + length;
	return base;
}

uint64_t vas_map_phys(vas_t *vas, uint64_t hint, uint64_t phys, size_t length,
					  uint64_t flags)
{
	assert(vas);
	if (!length)
		return 0;

	length = ALIGN_UP(length, PAGE_SIZE);
	phys = ALIGN_DOWN(phys, PAGE_SIZE);

	uint64_t base;
	if (flags & VAD_FIXED) {
		base = ALIGN_DOWN(hint, PAGE_SIZE);
		uint64_t end = base + length;
		if (end > VAS_USER_END || end < base)
			return 0;
		if (_vas_overlaps(vas, base, end))
			return 0;
	} else {
		uint64_t search_hint = hint ? hint : vas->user_start;
		base = _vas_find_free(vas, search_hint, length);
		if (!base)
			return 0;
	}

	uint64_t prot = flags & VAD_PROT_MASK;
	for (size_t off = 0; off < length; off += PAGE_SIZE) {
		uint64_t p = phys + off;
		page_t *page = pfndb_phys_to_page(p);
		if (page) {
			map_page(vas->pml4, base + off, page, prot);
		} else {
			map_page_phys(vas->pml4, base + off, p, prot);
		}
	}

	vad_t *vad = _vad_alloc(base, base + length, flags & ~VAD_ANONYMOUS);
	if (!vad) {
		for (size_t off = 0; off < length; off += PAGE_SIZE)
			unmap_page(vas->pml4, base + off);
		return 0;
	}

	_vad_insert(vas, vad);
	return base;
}

uint64_t vas_map_file(vas_t *vas, uint64_t hint, struct vfs_node *file,
					  uint64_t file_offset, size_t length, uint64_t flags)
{
	assert(vas);
	if (!file || !length)
		return 0;
	if ((file_offset & (PAGE_SIZE - 1)) != 0)
		return 0;

	length = ALIGN_UP(length, PAGE_SIZE);

	uint64_t base;
	if (flags & VAD_FIXED) {
		base = ALIGN_DOWN(hint, PAGE_SIZE);
		uint64_t end = base + length;
		if (end > VAS_USER_END || end < base)
			return 0;
		if (_vas_overlaps(vas, base, end))
			return 0;
	} else {
		uint64_t search_hint = hint ? hint : vas->user_start;
		base = _vas_find_free(vas, search_hint, length);
		if (!base)
			return 0;
	}

	vad_t *vad = _vad_alloc_file(base, base + length, flags, file, file_offset);
	if (!vad)
		return 0;

	if (_commit_file(vas, vad) != 0) {
		_uncommit_range(vas, base, base + length);
		_vad_free(vad);
		return 0;
	}

	_vad_insert(vas, vad);
	vas->user_start = base + length;
	return base;
}

int vas_unmap(vas_t *vas, uint64_t start, size_t length)
{
	assert(vas);
	if (!length)
		return 0;

	start = ALIGN_DOWN(start, PAGE_SIZE);
	length = ALIGN_UP(length, PAGE_SIZE);
	uint64_t end = start + length;

	for (vad_t *v = vas->list_head; v && v->start < end; v = v->next) {
		if (v->end <= start)
			continue;
		if (v->flags & VAD_ANONYMOUS) {
			uint64_t lo = v->start > start ? v->start : start;
			uint64_t hi = v->end < end ? v->end : end;
			_uncommit_range(vas, lo, hi);
		} else {
			uint64_t lo = v->start > start ? v->start : start;
			uint64_t hi = v->end < end ? v->end : end;
			for (uint64_t va = lo; va < hi; va += PAGE_SIZE)
				unmap_page(vas->pml4, va);
		}
	}

	_vad_remove_range(vas, start, end);
	return 0;
}

int vas_protect(vas_t *vas, uint64_t start, size_t length, uint64_t new_prot)
{
	assert(vas);
	if (!length)
		return 0;

	start = ALIGN_DOWN(start, PAGE_SIZE);
	length = ALIGN_UP(length, PAGE_SIZE);
	uint64_t end = start + length;

	new_prot &= VAD_PROT_MASK;

	for (vad_t *v = vas->list_head; v && v->start < end; v = v->next) {
		if (v->end <= start)
			continue;

		uint64_t lo = v->start > start ? v->start : start;
		uint64_t hi = v->end < end ? v->end : end;

		for (uint64_t va = lo; va < hi; va += PAGE_SIZE) {
			uint64_t phys = get_phys(vas->pml4, va);
			if (!phys)
				continue;

			page_t *page = pfndb_phys_to_page(phys);
			if (page) {
				page_ref(page);
				unmap_page(vas->pml4, va);
				map_page(vas->pml4, va, page, new_prot);
				page_unref(page);
			} else {
				unmap_page(vas->pml4, va);
				map_page_phys(vas->pml4, va, phys, new_prot);
			}
		}

		v->flags = (v->flags & ~VAD_PROT_MASK) | new_prot;
	}

	return 0;
}

vad_t *vas_find(vas_t *vas, uint64_t addr)
{
	assert(vas);
	for (vad_t *v = vas->list_head; v; v = v->next) {
		if (addr >= v->start && addr < v->end)
			return v;
	}
	return NULL;
}

int vas_range_mapped(vas_t *vas, uint64_t start, size_t length)
{
	assert(vas);
	if (!length)
		return 0;

	start = ALIGN_DOWN(start, PAGE_SIZE);
	length = ALIGN_UP(length, PAGE_SIZE);
	uint64_t end = start + length;
	if (end < start)
		return 0;

	uint64_t pos = start;
	for (vad_t *v = vas->list_head; v && pos < end; v = v->next) {
		if (v->end <= pos)
			continue;
		if (v->start > pos)
			return 0;
		if (v->end > pos)
			pos = v->end;
	}

	return pos >= end;
}

void vas_switch(vas_t *vas)
{
	assert(vas && vas->pml4);
	asm volatile("mov %0, %%cr3" ::"r"((uint64_t)vas->pml4) : "memory");
}

vas_t *vas_adopt(ptable_t *existing_pml4)
{
	assert(existing_pml4);
	vas_t *vas = kzalloc(sizeof(vas_t));
	if (!vas)
		return NULL;
	vas->pml4 = existing_pml4;
	vas->list_head = NULL;
	vas->user_start = VAS_USER_START;
	return vas;
}

int vas_handle_page_fault(vas_t *vas, uint64_t addr, uint64_t err)
{
	if (!vas)
		return -1;

	uint64_t va = ALIGN_DOWN(addr, PAGE_SIZE);
	vad_t *vad = vas_find(vas, va);
	if (!vad)
		return -1;

	if (!(err & 0x2) || !(err & 0x1))
		return -1;
	if (!(vad->flags & VMM_WRITABLE) || (vad->flags & VAD_SHARED))
		return -1;

	uint64_t phys = get_phys(vas->pml4, va);
	if (!phys)
		return -1;

	page_t *old_page = pfndb_phys_to_page(ALIGN_DOWN(phys, PAGE_SIZE));
	if (!old_page || !page_is_cow(old_page))
		return -1;

	uint64_t prot = vad->flags & VAD_PROT_MASK;
	if (!page_is_shared(old_page)) {
		page_ref(old_page);
		unmap_page(vas->pml4, va);
		page_clear_cow(old_page);
		map_page(vas->pml4, va, old_page, prot);
		page_unref(old_page);
		return 0;
	}

	page_t *new_page = palloc_page();
	if (!new_page)
		return -1;

	uint64_t new_phys = pfndb_page_to_phys(new_page);
	memcpy(PHYS_TO_VIRT(new_phys), PHYS_TO_VIRT(ALIGN_DOWN(phys, PAGE_SIZE)),
		   PAGE_SIZE);

	unmap_page(vas->pml4, va);
	map_page(vas->pml4, va, new_page, prot);
	page_unref(new_page);
	return 0;
}

int vas_user_access_ok(vas_t *vas, uint64_t addr, size_t len, int write)
{
	if (!vas)
		return -1;
	if (addr < VAS_USER_START || addr >= VAS_USER_END)
		return -1;
	if (len > VAS_USER_END - addr)
		return -1;
	if (len == 0)
		return 0;

	uint64_t start = ALIGN_DOWN(addr, PAGE_SIZE);
	uint64_t end = ALIGN_UP(addr + len, PAGE_SIZE);

	for (uint64_t va = start; va < end; va += PAGE_SIZE) {
		uint64_t flags = get_mapping_flags(vas->pml4, va);
		if (!(flags & VMM_PRESENT))
			return -1;
		if (write && !(flags & VMM_WRITABLE)) {
			if (vas_handle_page_fault(vas, va, 0x3) != 0)
				return -1;
			flags = get_mapping_flags(vas->pml4, va);
			if (!(flags & VMM_PRESENT) || !(flags & VMM_WRITABLE))
				return -1;
		}
	}

	return 0;
}


vas_t *vas_clone(vas_t *src)
{
	if (!src)
		return NULL;

	vas_t *dst = vas_create(NULL);
	if (!dst)
		return NULL;
	dst->user_start = src->user_start;

	for (vad_t *v = src->list_head; v; v = v->next) {
		vad_t *nv = NULL;
		if (v->flags & VAD_FILE)
			nv = _vad_alloc_file(v->start, v->end, v->flags, v->file,
								 v->file_offset);
		else
			nv = _vad_alloc(v->start, v->end, v->flags);
		if (!nv) {
			vas_destroy(dst);
			return NULL;
		}
		_vad_insert(dst, nv);

		uint64_t prot = v->flags & VAD_PROT_MASK;
		for (uint64_t va = v->start; va < v->end; va += PAGE_SIZE) {
			uint64_t src_phys = get_phys(src->pml4, va);
			if (!src_phys)
				continue;

			uint64_t src_page_phys = ALIGN_DOWN(src_phys, PAGE_SIZE);
			page_t *page = pfndb_phys_to_page(src_page_phys);
			uint64_t map_flags = prot;

			if (page) {
				if ((prot & VMM_WRITABLE) && !(v->flags & VAD_SHARED)) {
					uint64_t cow_flags = prot & ~VMM_WRITABLE;

					page_ref(page);
					unmap_page(src->pml4, va);
					map_page(src->pml4, va, page, cow_flags);
					page_unref(page);
					page_mark_cow(page);

					map_flags = cow_flags;
				}

				map_page(dst->pml4, va, page, map_flags);
				continue;
			}

			map_page_phys(dst->pml4, va, src_page_phys, map_flags);
		}
	}

	return dst;
}
