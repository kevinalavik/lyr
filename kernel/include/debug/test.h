#ifndef _LYR_DEBUG_TEST_H
#define _LYR_DEBUG_TEST_H

#include <mm/heap.h>
#include <mm/pmm.h>
#include <debug/log.h>
#include <debug/assert.h>
#include <mm/pfndb.h>
#include <mm/page.h>
#include <lib/string.h>

static void pmm_test(void)
{
	/* 1. LIFO order */
	{
		page_t *a = palloc_page();
		page_t *b = palloc_page();
		uint64_t phys_a = pfndb_page_to_phys(a);
		uint64_t phys_b = pfndb_page_to_phys(b);

		page_unref(a);
		page_unref(b);

		page_t *c = palloc_page();
		page_t *d = palloc_page();

		assert(pfndb_page_to_phys(c) == phys_b);
		assert(pfndb_page_to_phys(d) == phys_a);

		page_unref(c);
		page_unref(d);
		log_debug("pmm_test", "LIFO order ok (a=0x%llx b=0x%llx)", phys_a,
				  phys_b);
	}

	/* 2. refcount */
	{
		page_t *p = palloc_page();
		assert(p->refcount == 1);

		page_ref(p);
		assert(p->refcount == 2);
		assert(page_is_used(p));

		page_unref(p);
		assert(p->refcount == 1);
		assert(page_is_used(p));

		page_unref(p);
		assert(p->refcount == 0);
		assert(page_is_free(p));
		log_debug("pmm_test", "refcount ok");
	}

	/* 3. sharecount + PAGE_SHARED flag */
	{
		page_t *p = palloc_page();
		assert(p->u2.sharecount == 0);
		assert(!page_is_shared(p));

		page_share(p);
		assert(p->u2.sharecount == 1);
		assert(!page_is_shared(p));

		page_share(p);
		assert(p->u2.sharecount == 2);
		assert(page_is_shared(p));

		page_unshare(p);
		assert(p->u2.sharecount == 1);
		assert(!page_is_shared(p));

		page_unshare(p);
		assert(p->u2.sharecount == 0);

		page_unref(p);
		log_debug("pmm_test", "sharecount ok");
	}

	/* 4. scrub on free */
	{
		page_t *p = palloc_page();
		char *virt = PHYS_TO_VIRT(pfndb_page_to_phys(p));
		memset(virt, 0xAB, PAGE_SIZE);
		page_unref(p);

		page_t *q = palloc_page();
		assert(pfndb_page_to_phys(q) == pfndb_page_to_phys(p));

		char *virt2 = PHYS_TO_VIRT(pfndb_page_to_phys(q));
		for (int i = 0; i < (int)PAGE_SIZE; i++)
			assert(virt2[i] == 0);

		page_unref(q);
		log_debug("pmm_test", "scrub ok");
	}

	/* 5. HHDM write */
	{
		page_t *p = palloc_page();
		char *virt = PHYS_TO_VIRT(pfndb_page_to_phys(p));
		*virt = 'Z';
		assert(*virt == 'Z');
		assert(p->refcount == 1);
		page_unref(p);
		log_debug("pmm_test", "HHDM write ok");
	}

	log_debug("pmm_test", "all tests passed");
}

static void paging_test(void)
{
	uint64_t cr3 = read_cr3();
	ptable_t *pt = (ptable_t *)cr3;

	/* 1. basic map, write, read, unmap */
	{
		uint64_t virt = 0xdeadbeef000ULL;
		page_t *page = palloc_page();

		map_page(pt, virt, page, VMM_FLAGS_KERNEL_RW);
		assert(page->u2.sharecount == 1);
		assert(page->refcount == 2); /* palloc gave 1, map_page added 1 */

		volatile char *ptr = (volatile char *)virt;
		*ptr = 'A';
		assert(*ptr == 'A');
		log_debug("paging_test",
				  "map+write ok: virt=0x%llx phys=0x%llx val='%c'", virt,
				  pfndb_page_to_phys(page), *ptr);

		unmap_page(pt, virt);
		assert(page->u2.sharecount == 0);
		assert(page->refcount == 1);
		log_debug("paging_test", "unmap ok");

		page_unref(page);
	}

	/* 2. get_phys */
	{
		uint64_t virt = 0xcafe0000ULL;
		page_t *page = palloc_page();
		uint64_t phys = pfndb_page_to_phys(page);

		map_page(pt, virt, page, VMM_FLAGS_KERNEL_RW);
		assert(get_phys(pt, virt) == phys);
		assert(get_phys(pt, virt + 1) == phys + 1);
		log_debug("paging_test", "get_phys ok: 0x%llx -> 0x%llx", virt, phys);

		unmap_page(pt, virt);
		assert(get_phys(pt, virt) == 0);
		page_unref(page);
	}

	/* 3. double-map same page into two virtual addresses */
	{
		uint64_t virt1 = 0xf000000000ULL;
		uint64_t virt2 = 0xf000001000ULL;
		page_t *page = palloc_page();

		map_page(pt, virt1, page, VMM_FLAGS_KERNEL_RW);
		map_page(pt, virt2, page, VMM_FLAGS_KERNEL_RW);
		assert(page->u2.sharecount == 2);
		assert(page_is_shared(page));

		volatile char *p1 = (volatile char *)virt1;
		volatile char *p2 = (volatile char *)virt2;
		*p1 = 'X';
		assert(*p2 == 'X'); /* same physical page, both should read 'X' */
		log_debug("paging_test", "shared mapping ok: virt1=0x%llx virt2=0x%llx",
				  virt1, virt2);

		unmap_page(pt, virt1);
		assert(page->u2.sharecount == 1);
		assert(!page_is_shared(page));

		unmap_page(pt, virt2);
		assert(page->u2.sharecount == 0);
		assert(page->refcount == 1);

		page_unref(page);
	}

	log_debug("paging_test", "all tests passed");
}

static void heap_test(void)
{
	log_debug("heap_test", "starting");

	/* 1. basic alloc/free (small sizes) */
	{
		void *a = kmalloc(16);
		void *b = kmalloc(32);
		assert(a && b);

		memset(a, 0xAA, 16);
		memset(b, 0xBB, 32);

		kfree(a);
		kfree(b);

		log_debug("heap_test", "basic alloc/free ok");
	}

	/* 2. slab reuse */
	{
		void *a = kmalloc(64);
		kfree(a);

		void *b = kmalloc(64);
		assert(b != NULL);

		kfree(b);
		log_debug("heap_test", "slab reuse ok");
	}

	/* 3. fill slab completely */
	{
		size_t sz = 128;
		void *objs[128];

		for (int i = 0; i < 128; i++) {
			objs[i] = kmalloc(sz);
			assert(objs[i]);
		}

		for (int i = 0; i < 128; i++)
			kfree(objs[i]);

		log_debug("heap_test", "slab fill/free ok");
	}

	/* 4. write/read correctness */
	{
		char *p = kmalloc(256);
		assert(p);

		for (int i = 0; i < 256; i++)
			p[i] = (char)i;

		for (int i = 0; i < 256; i++)
			assert(p[i] == (char)i);

		kfree(p);
		log_debug("heap_test", "write/read ok");
	}

	/* 5. large allocation */
	{
		size_t big = PAGE_SIZE * 3;
		void *p = kmalloc(big);
		assert(p);

		memset(p, 0xCC, big);

		for (size_t i = 0; i < big; i++)
			assert(((uint8_t *)p)[i] == 0xCC);

		kfree(p);
		log_debug("heap_test", "large alloc ok");
	}

	/* 6. krealloc grow */
	{
		char *p = kmalloc(64);
		assert(p);

		strcpy(p, "hello world");

		p = krealloc(p, 512);
		assert(p);
		assert(strcmp(p, "hello world") == 0);

		kfree(p);
		log_debug("heap_test", "realloc grow ok");
	}

	/* 7. krealloc shrink */
	{
		char *p = kmalloc(512);
		assert(p);

		strcpy(p, "shrink test");

		p = krealloc(p, 32);
		assert(p);
		assert(strcmp(p, "shrink test") == 0);

		kfree(p);
		log_debug("heap_test", "realloc shrink ok");
	}

	/* 8. kzalloc */
	{
		uint8_t *p = kzalloc(128);
		assert(p);

		for (int i = 0; i < 128; i++)
			assert(p[i] == 0);

		kfree(p);
		log_debug("heap_test", "kzalloc zeroing ok");
	}

	log_debug("heap_test", "all tests passed");
}

static void vmm_test(vas_t *vas)
{
	log_debug("vmm_test", "starting");

	uint64_t free_before = pmm_free_pages();

	/* 1. basic anonymous map, write, unmap */
	{
		uint64_t addr =
			vas_map_anon(vas, 0, PAGE_SIZE, VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);
		log_debug("vmm_test", "1: anon map at 0x%llx", addr);

		vad_t *v = vas_find(vas, addr);
		assert(v != NULL);
		assert(v->start == addr);
		assert(v->end == addr + PAGE_SIZE);
		assert(v->flags & VAD_ANONYMOUS);

		volatile uint64_t *p = (volatile uint64_t *)addr;
		*p = 0xCAFEBABEDEADBEEFull;
		assert(*p == 0xCAFEBABEDEADBEEFull);

		uint64_t phys = get_phys(vas->pml4, addr);
		assert(phys != 0);
		log_debug("vmm_test", "1: phys=0x%llx val=0x%llx ok", phys, *p);

		vas_unmap(vas, addr, PAGE_SIZE);
		assert(vas_find(vas, addr) == NULL);
		assert(get_phys(vas->pml4, addr) == 0);

		log_debug("vmm_test", "1: anon map/write/unmap ok");
	}

	/* 2. multi-page anonymous map */
	{
		size_t len = PAGE_SIZE * 4;
		uint64_t addr = vas_map_anon(vas, 0, len, VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);

		for (size_t off = 0; off < len; off += PAGE_SIZE) {
			volatile uint8_t *p = (volatile uint8_t *)(addr + off);
			*p = (uint8_t)(off / PAGE_SIZE);
			assert(*p == (uint8_t)(off / PAGE_SIZE));
			assert(get_phys(vas->pml4, addr + off) != 0);
		}

		vas_unmap(vas, addr, len);

		for (size_t off = 0; off < len; off += PAGE_SIZE)
			assert(get_phys(vas->pml4, addr + off) == 0);

		log_debug("vmm_test", "2: multi-page anon ok");
	}

	/* 3. VAD_FIXED placement */
	{
		uint64_t want = 0x0000500000000000ull;
		uint64_t addr = vas_map_anon(vas, want, PAGE_SIZE,
									 VMM_PRESENT | VMM_WRITABLE | VAD_FIXED);
		assert(addr == want);

		vad_t *v = vas_find(vas, want);
		assert(v && v->start == want);

		volatile uint32_t *p = (volatile uint32_t *)want;
		*p = 0xABCD1234u;
		assert(*p == 0xABCD1234u);

		vas_unmap(vas, want, PAGE_SIZE);
		assert(vas_find(vas, want) == NULL);

		log_debug("vmm_test", "3: VAD_FIXED ok");
	}

	/* 4. sequential allocations don't overlap */
	{
		uint64_t a =
			vas_map_anon(vas, 0, PAGE_SIZE, VMM_PRESENT | VMM_WRITABLE);
		uint64_t b =
			vas_map_anon(vas, 0, PAGE_SIZE * 2, VMM_PRESENT | VMM_WRITABLE);
		uint64_t c =
			vas_map_anon(vas, 0, PAGE_SIZE, VMM_PRESENT | VMM_WRITABLE);

		assert(a && b && c);
		assert(a != b && b != c && a != c);
		assert(a + PAGE_SIZE <= b || b + PAGE_SIZE * 2 <= a);
		assert(b + PAGE_SIZE * 2 <= c || c + PAGE_SIZE <= b);
		assert(a + PAGE_SIZE <= c || c + PAGE_SIZE <= a);
		log_debug("vmm_test", "4: a=0x%llx b=0x%llx c=0x%llx", a, b, c);

		vas_unmap(vas, a, PAGE_SIZE);
		vas_unmap(vas, b, PAGE_SIZE * 2);
		vas_unmap(vas, c, PAGE_SIZE);

		log_debug("vmm_test", "4: no-overlap ok");
	}

	/* 5. vas_protect strips WRITABLE, data survives */
	{
		uint64_t addr =
			vas_map_anon(vas, 0, PAGE_SIZE, VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);

		volatile uint64_t *p = (volatile uint64_t *)addr;
		*p = 0x1234567890ABCDEFull;
		assert(*p == 0x1234567890ABCDEFull);

		vas_protect(vas, addr, PAGE_SIZE, VMM_PRESENT);

		vad_t *v = vas_find(vas, addr);
		assert(v);
		assert(!(v->flags & VMM_WRITABLE));
		assert(get_phys(vas->pml4, addr) != 0);
		assert(*p == 0x1234567890ABCDEFull);
		log_debug("vmm_test", "5: protect ok, value=0x%llx", *p);

		vas_unmap(vas, addr, PAGE_SIZE);

		log_debug("vmm_test", "5: vas_protect ok");
	}

	/* 6. vas_map_phys — verify same bytes visible through both windows */
	{
		page_t *pg = palloc_page();
		uint64_t phys = pfndb_page_to_phys(pg);

		uint8_t *hhdm = (uint8_t *)PHYS_TO_VIRT(phys);
		for (int i = 0; i < 16; i++)
			hhdm[i] = (uint8_t)(0xA0 + i);

		uint64_t mapped =
			vas_map_phys(vas, 0, phys, PAGE_SIZE, VMM_PRESENT | VMM_WRITABLE);
		assert(mapped != 0);
		log_debug("vmm_test", "6: phys=0x%llx mapped at 0x%llx", phys, mapped);

		uint8_t *via_vas = (uint8_t *)mapped;
		for (int i = 0; i < 16; i++) {
			assert(via_vas[i] == hhdm[i]);
			log_debug("vmm_test", "6: [%d] hhdm=0x%02x vas=0x%02x", i, hhdm[i],
					  via_vas[i]);
		}

		vas_unmap(vas, mapped, PAGE_SIZE);
		assert(get_phys(vas->pml4, mapped) == 0);

		assert(page_is_used(pg));
		page_unref(pg);

		log_debug("vmm_test", "6: vas_map_phys ok");
	}

	/* 7. partial unmap splits VAD into left + right remainders */
	{
		size_t len = PAGE_SIZE * 4;
		uint64_t addr = vas_map_anon(vas, 0, len, VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);

		vas_unmap(vas, addr + PAGE_SIZE, PAGE_SIZE * 2);

		vad_t *left = vas_find(vas, addr);
		assert(left && left->start == addr && left->end == addr + PAGE_SIZE);

		vad_t *right = vas_find(vas, addr + PAGE_SIZE * 3);
		assert(right && right->start == addr + PAGE_SIZE * 3 &&
			   right->end == addr + PAGE_SIZE * 4);

		assert(get_phys(vas->pml4, addr + PAGE_SIZE) == 0);
		assert(get_phys(vas->pml4, addr + PAGE_SIZE * 2) == 0);
		assert(get_phys(vas->pml4, addr) != 0);
		assert(get_phys(vas->pml4, addr + PAGE_SIZE * 3) != 0);

		vas_unmap(vas, addr, PAGE_SIZE);
		vas_unmap(vas, addr + PAGE_SIZE * 3, PAGE_SIZE);

		log_debug("vmm_test", "7: partial unmap / VAD split ok");
	}

	/* 8. pages returned to PMM after full unmap */
	{
		uint64_t snap = pmm_free_pages();
		size_t npages = 8;
		uint64_t addr = vas_map_anon(vas, 0, PAGE_SIZE * npages,
									 VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);

		uint64_t after_map = pmm_free_pages();
		assert(after_map < snap);
		assert(snap - after_map >= npages);

		vas_unmap(vas, addr, PAGE_SIZE * npages);
		assert(pmm_free_pages() == snap);
		log_debug("vmm_test", "8: %llu pages reclaimed ok", (uint64_t)npages);
	}

	/* 9. unique per-word pattern across 32 regions, full read-back */
	{
#define T9_COUNT 32
#define T9_SIZE (PAGE_SIZE * 3)
		void *r[T9_COUNT];
		for (int i = 0; i < T9_COUNT; i++) {
			r[i] = (void *)vas_map_anon(vas, 0, T9_SIZE,
										VMM_PRESENT | VMM_WRITABLE);
			assert(r[i] != NULL);
			uint64_t *p = (uint64_t *)r[i];
			for (size_t j = 0; j < T9_SIZE / sizeof(uint64_t); j++)
				p[j] = 0xABCD000000000000ull ^ ((uint64_t)i << 32) ^ j;
		}
		for (int i = 0; i < T9_COUNT; i++) {
			uint64_t *p = (uint64_t *)r[i];
			for (size_t j = 0; j < T9_SIZE / sizeof(uint64_t); j++) {
				uint64_t expected =
					0xABCD000000000000ull ^ ((uint64_t)i << 32) ^ j;
				assert(p[j] == expected);
			}
			vas_unmap(vas, (uint64_t)r[i], T9_SIZE);
		}
		log_debug("vmm_test", "9: multi-region pattern verify ok");
#undef T9_COUNT
#undef T9_SIZE
	}

	/* 10. interleaved free/realloc — aliasing detection */
	{
#define T10_COUNT 16
#define T10_SIZE PAGE_SIZE
		void *r[T10_COUNT];
		for (int i = 0; i < T10_COUNT; i++) {
			r[i] = (void *)vas_map_anon(vas, 0, T10_SIZE,
										VMM_PRESENT | VMM_WRITABLE);
			assert(r[i] != NULL);
			*(volatile uint64_t *)r[i] = 0xFEEDFACE00000000ull | (uint64_t)i;
		}
		/* free evens, reallocate with inverted sentinel */
		for (int i = 0; i < T10_COUNT; i += 2) {
			vas_unmap(vas, (uint64_t)r[i], T10_SIZE);
			r[i] = (void *)vas_map_anon(vas, 0, T10_SIZE,
										VMM_PRESENT | VMM_WRITABLE);
			assert(r[i] != NULL);
			*(volatile uint64_t *)r[i] = ~(0xFEEDFACE00000000ull | (uint64_t)i);
		}
		/* odd regions must be untouched */
		for (int i = 1; i < T10_COUNT; i += 2) {
			uint64_t expected = 0xFEEDFACE00000000ull | (uint64_t)i;
			assert(*(volatile uint64_t *)r[i] == expected);
		}
		for (int i = 0; i < T10_COUNT; i++)
			vas_unmap(vas, (uint64_t)r[i], T10_SIZE);
		log_debug("vmm_test", "10: interleaved free/realloc aliasing ok");
#undef T10_COUNT
#undef T10_SIZE
	}

	/* 11. vas_protect RW->RO->RW round-trip, data intact */
	{
		uint64_t addr =
			vas_map_anon(vas, 0, PAGE_SIZE * 2, VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);

		uint64_t *p = (uint64_t *)addr;
		size_t words = (PAGE_SIZE * 2) / sizeof(uint64_t);
		for (size_t j = 0; j < words; j++)
			p[j] = 0xC0FFEE00ull ^ j;

		vas_protect(vas, addr, PAGE_SIZE * 2, VMM_PRESENT);
		vas_protect(vas, addr, PAGE_SIZE * 2, VMM_PRESENT | VMM_WRITABLE);

		for (size_t j = 0; j < words; j++)
			assert(p[j] == (0xC0FFEE00ull ^ j));

		vad_t *v = vas_find(vas, addr);
		assert(v && (v->flags & VMM_WRITABLE));
		assert(get_phys(vas->pml4, addr) != 0);
		assert(get_phys(vas->pml4, addr + PAGE_SIZE) != 0);

		vas_unmap(vas, addr, PAGE_SIZE * 2);
		log_debug("vmm_test", "11: protect round-trip ok");
	}

	/* 12. double-unmap must not crash or corrupt VAD list */
	{
		uint64_t addr =
			vas_map_anon(vas, 0, PAGE_SIZE, VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);
		vas_unmap(vas, addr, PAGE_SIZE);
		vas_unmap(vas, addr, PAGE_SIZE); /* no-op, must not panic */
		assert(vas_find(vas, addr) == NULL);
		log_debug("vmm_test", "12: double-unmap ok");
	}

	/* 13. zero-length map must fail; size-1 must round up and succeed */
	{
		uint64_t r = vas_map_anon(vas, 0, 0, VMM_PRESENT | VMM_WRITABLE);
		assert(r == 0);

		uint64_t addr = vas_map_anon(vas, 0, 1, VMM_PRESENT | VMM_WRITABLE);
		assert(addr != 0);
		assert((addr & 0xFFF) == 0);
		*(volatile uint8_t *)addr = 0xBE;
		assert(*(volatile uint8_t *)addr == 0xBE);
		vas_unmap(vas, addr, 1);
		log_debug("vmm_test", "13: zero-len fail / sub-page round-up ok");
	}

	/* 14. VAD_FIXED collision must fail */
	{
		uint64_t want = 0x0000480000000000ull;
		uint64_t first = vas_map_anon(vas, want, PAGE_SIZE,
									  VMM_PRESENT | VMM_WRITABLE | VAD_FIXED);
		assert(first == want);

		uint64_t collision = vas_map_anon(
			vas, want, PAGE_SIZE, VMM_PRESENT | VMM_WRITABLE | VAD_FIXED);
		assert(collision == 0);

		vas_unmap(vas, want, PAGE_SIZE);
		log_debug("vmm_test", "14: VAD_FIXED collision rejected ok");
	}

	/* 15. PMM leak check — net pages consumed must be zero */
	{
		uint64_t free_after = pmm_free_pages();
		log_debug("vmm_test",
				  "15: free pages before=%llu after=%llu delta=%lld",
				  free_before, free_after,
				  (int64_t)free_after - (int64_t)free_before);
		assert(free_after == free_before);
		log_debug("vmm_test", "15: no PMM leak ok");
	}

	log_debug("vmm_test", "all tests passed");
}

#endif // _LYR_DEBUG_TEST_H