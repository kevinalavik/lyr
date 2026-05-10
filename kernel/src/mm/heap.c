#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/page.h>
#include <lib/string.h>
#include <stddef.h>
#include <stdint.h>
#include <mm/pfndb.h>
#include <mm/paging.h>
#include <mm/vmm.h>
#include <sync/spinlock.h>

#define SLAB_MIN 8u
#define SLAB_MAX 2048u
#define SLAB_ALIGN 8u
#define NUM_CACHES 9u
#define LARGE_REGION_BASE 0xffffffffb0000000ULL

#define LARGE_MAGIC 0x4C524748ul

typedef struct slab slab_t;
struct slab {
	slab_t *next;
	void *free;
	uint32_t total;
	uint32_t used;
	uint32_t obj_sz;
	uint32_t _pad;
};

typedef struct {
	slab_t *partial;
	slab_t *full;
	size_t obj_size;
	spinlock_t lock;
} cache_t;

typedef struct {
	uint32_t magic;
	uint32_t _pad;
	uint64_t npages;
} lhdr_t;

static cache_t caches[NUM_CACHES];
static spinlock_t large_lock = SPINLOCK_INIT;
static uint64_t next_large_base = LARGE_REGION_BASE;

static inline size_t align_up(size_t v, size_t a)
{
	return (v + a - 1) & ~(a - 1);
}

static inline unsigned cache_idx(size_t size)
{
	if (size < SLAB_MIN)
		size = SLAB_MIN;
	size_t s = SLAB_MIN;
	unsigned i = 0;
	while (s < size && i < NUM_CACHES - 1) {
		s <<= 1;
		i++;
	}
	return (s >= size) ? i : NUM_CACHES;
}

static inline slab_t *obj_to_slab(void *obj)
{
	return (slab_t *)((uintptr_t)obj & ~(uintptr_t)(PAGE_SIZE - 1));
}

static int slab_unlink(slab_t **head, slab_t *s)
{
	for (slab_t **c = head; *c; c = &(*c)->next) {
		if (*c == s) {
			*c = s->next;
			s->next = NULL;
			return 1;
		}
	}
	return 0;
}

static slab_t *slab_create(size_t obj_size)
{
	void *page = PHYS_TO_VIRT(palloc_single());
	if (!page)
		return NULL;

	size_t stride = align_up(obj_size, SLAB_ALIGN);
	if (stride < sizeof(void *))
		stride = sizeof(void *);

	slab_t *s = (slab_t *)page;
	s->next = NULL;
	s->obj_sz = (uint32_t)stride;
	s->used = 0;

	uintptr_t area = align_up((uintptr_t)page + sizeof(slab_t), SLAB_ALIGN);
	uintptr_t end = (uintptr_t)page + PAGE_SIZE;

	s->total = (uint32_t)((end - area) / stride);
	s->free = (void *)area;

	for (uint32_t i = 0; i < s->total; i++) {
		void *o = (void *)(area + i * stride);
		*(void **)o =
			(i + 1 < s->total) ? (void *)(area + (i + 1) * stride) : NULL;
	}

	return s;
}

static void *slab_obj_alloc(slab_t *s)
{
	if (!s->free)
		return NULL;
	void *o = s->free;
	s->free = *(void **)o;
	s->used++;
	return o;
}

static void slab_obj_free(slab_t *s, void *o)
{
	*(void **)o = s->free;
	s->free = o;
	s->used--;
}

static void *cache_alloc(cache_t *c)
{
	spinlock_acquire(&c->lock);
	if (!c->partial) {
		slab_t *s = slab_create(c->obj_size);
		if (!s) {
			spinlock_release(&c->lock);
			return NULL;
		}
		s->next = c->partial;
		c->partial = s;
	}

	slab_t *s = c->partial;
	void *o = slab_obj_alloc(s);

	if (s->used == s->total) {
		c->partial = s->next;
		s->next = c->full;
		c->full = s;
	}

	spinlock_release(&c->lock);
	return o;
}

static void cache_free(cache_t *c, void *o)
{
	spinlock_acquire(&c->lock);
	slab_t *s = obj_to_slab(o);
	int was_full = (s->used == s->total);

	slab_obj_free(s, o);

	if (was_full) {
		slab_unlink(&c->full, s);
		s->next = c->partial;
		c->partial = s;
	}

	if (s->used == 0) {
		slab_unlink(&c->partial, s);

		uint64_t phys = VIRT_TO_PHYS(s);
		page_t *page = pfndb_phys_to_page(phys);

		page_unref(page);
	}
	spinlock_release(&c->lock);
}

static void *large_alloc(size_t size)
{
	uint64_t total = size + PAGE_SIZE;
	uint64_t npages = (total + PAGE_SIZE - 1) / PAGE_SIZE;

	spinlock_acquire(&large_lock);
	uint64_t base = next_large_base;
	next_large_base += npages * PAGE_SIZE;
	spinlock_release(&large_lock);

	for (uint64_t i = 0; i < npages; i++) {
		page_t *page = palloc_page();
		if (!page)
			return NULL;
		map_page(kernel_ptable, base + i * PAGE_SIZE, page,
				 VMM_PRESENT | VMM_WRITABLE | VMM_NX);
		page_unref(page);
	}

	lhdr_t *hdr = (lhdr_t *)base;
	hdr->magic = LARGE_MAGIC;
	hdr->npages = npages;
	return (void *)(base + PAGE_SIZE);
}

static void large_free(void *ptr)
{
	lhdr_t *hdr = (lhdr_t *)((uintptr_t)ptr - PAGE_SIZE);
	if (hdr->magic != LARGE_MAGIC)
		return;

	uint64_t base = (uint64_t)hdr;
	uint64_t npages = hdr->npages;
	hdr->magic = 0;
	for (uint64_t i = 0; i < npages; i++)
		unmap_page(kernel_ptable, base + i * PAGE_SIZE);
}

void kheap_init(void)
{
	size_t size = SLAB_MIN;
	for (unsigned i = 0; i < NUM_CACHES; i++) {
		caches[i] =
			(cache_t){ .partial = NULL, .full = NULL, .obj_size = size, .lock = SPINLOCK_INIT };
		size <<= 1;
	}
}

void *kmalloc(size_t size)
{
	if (!size)
		return NULL;
	unsigned i = cache_idx(size);
	return (i < NUM_CACHES) ? cache_alloc(&caches[i]) : large_alloc(size);
}

void *kzalloc(size_t size)
{
	void *p = kmalloc(size);
	if (p)
		memset(p, 0, size);
	return p;
}

void kfree(void *ptr)
{
	if (!ptr)
		return;

	lhdr_t *hdr = (lhdr_t *)((uintptr_t)ptr - PAGE_SIZE);
	if (hdr->magic == LARGE_MAGIC) {
		large_free(ptr);
		return;
	}

	slab_t *s = obj_to_slab(ptr);
	unsigned i = cache_idx(s->obj_sz);
	if (i < NUM_CACHES)
		cache_free(&caches[i], ptr);
}

void *krealloc(void *ptr, size_t size)
{
	if (!ptr)
		return kmalloc(size);
	if (!size) {
		kfree(ptr);
		return NULL;
	}

	lhdr_t *hdr = (lhdr_t *)((uintptr_t)ptr - PAGE_SIZE);
	size_t old = (hdr->magic == LARGE_MAGIC) ?
					 (size_t)((hdr->npages - 1) * PAGE_SIZE) :
					 obj_to_slab(ptr)->obj_sz;

	if (cache_idx(size) == cache_idx(old))
		return ptr;

	void *n = kmalloc(size);
	if (!n)
		return NULL;
	memcpy(n, ptr, old < size ? old : size);
	kfree(ptr);
	return n;
}
