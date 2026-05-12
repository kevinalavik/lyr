#include <mm/pmm.h>
#include <mm/pfndb.h>
#include <mm/page.h>
#include <debug/log.h>
#include <debug/panic.h>
#include <debug/assert.h>
#include <lib/string.h>
#include <sync/spinlock.h>

#if _PMM_TRACE
#define _pmm_log_trace(...) log_trace(__VA_ARGS__)
#else
#define _pmm_log_trace(...) ((void)0)
#endif

static page_t *freelist = NULL;
static uint64_t free_pages = 0;
static uint64_t total_pages = 0;
static spinlock_t pmm_lock = SPINLOCK_INIT;

static uint64_t pmm_irq_save(void)
{
	uint64_t flags;
	__asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
	return flags;
}

static void pmm_irq_restore(uint64_t flags)
{
	if (flags & (1ull << 9))
		__asm__ volatile("sti" ::: "memory");
}

static void _page_validate_free(const page_t *page, const char *caller)
{
	if (!page)
		kpanic(NULL, "%s: NULL page", caller);
	if (page_is_poisoned(page)) {
		log_warn("pmm", "%s: touched poisoned page @ %p — skipping", caller,
				 page);
		return;
	}
	if (!page_is_free(page))
		kpanic(NULL, "%s: page not free (flags=0x%x)", caller, page->flags);
	if (page->refcount != 0)
		kpanic(NULL, "%s: free page has nonzero refcount=%u", caller,
			   page->refcount);
	if (page->u2.sharecount != 0)
		kpanic(NULL, "%s: free page has nonzero sharecount=%llu", caller,
			   page->u2.sharecount);
}

static void _page_validate_used(const page_t *page, const char *caller)
{
	if (!page)
		kpanic(NULL, "%s: NULL page", caller);
	if (page_is_poisoned(page))
		kpanic(NULL, "%s: touched poisoned page @ %p", caller, page);
	if (!page_is_used(page))
		kpanic(NULL, "%s: page not marked used (flags=0x%x)", caller,
			   page->flags);
	if (page->refcount == 0)
		kpanic(NULL, "%s: used page has zero refcount", caller);
}

/* init only */
static void _pmm_push_noscrub(page_t *page)
{
	assert(page);
	assert(page->refcount == 0);
	assert(page->u2.sharecount == 0);
	assert(!(page->flags & PAGE_FREE));
	assert(!(page->flags & PAGE_RESERVED));
	assert(!(page->flags & PAGE_POISON));

	page->flags = PAGE_FREE;
	page->u1.next = freelist;
	page->u2.prev = NULL;

	if (freelist)
		freelist->u2.prev = page;

	freelist = page;
	free_pages++;
}

static void _pmm_push(page_t *page)
{
	assert(page);
	assert(page->refcount == 0);
	assert(page->u2.sharecount == 0);
	assert(!(page->flags & PAGE_FREE));
	assert(!(page->flags & PAGE_RESERVED));
	assert(!(page->flags & PAGE_POISON));

	/* scrub before reuse */
	uint64_t phys = pfndb_page_to_phys(page);
	memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);

	page->flags = PAGE_FREE;

	/* insert at head: page <-> old_head */
	page->u1.next = freelist;
	page->u2.prev = NULL;

	if (freelist)
		freelist->u2.prev = page;

	freelist = page;
	free_pages++;
}

static page_t *_pmm_pop(void)
{
	if (!freelist) {
		log_warn("pmm", "_pmm_pop: freelist exhausted");
		return NULL;
	}

	page_t *page = freelist;
	_page_validate_free(page, "_pmm_pop");

	/* unlink head */
	freelist = page->u1.next;
	if (freelist)
		freelist->u2.prev = NULL;

	/* clear links */
	page->u1.next = NULL;
	page->u2.sharecount = 0; /* u2 reused: prev -> sharecount */

	/* FREE -> USED */
	page->flags &= ~PAGE_FREE;
	page->flags |= PAGE_USED;

	free_pages--;
	return page;
}

void pmm_init(void)
{
	page_t *pfndb = pfndb_getdb();
	uint64_t max_pfn = pfndb_getmax();

	assert(pfndb);

	spinlock_acquire(&pmm_lock);
	freelist = NULL;
	free_pages = 0;
	total_pages = max_pfn + 1;

	for (uint64_t i = max_pfn; i != (uint64_t)-1; i--) {
		page_t *page = pfndb_getptr(i);

		if (!(page->flags & PAGE_FREE))
			continue;
		if (page->flags & PAGE_RESERVED)
			continue;

		page->refcount = 0;
		page->u2.sharecount = 0;
		page->u1.next = NULL;
		page->u2.prev = NULL;
		page->flags &= ~PAGE_FREE;

		_pmm_push_noscrub(page);
	}
	spinlock_release(&pmm_lock);

	log_debug("pmm", "initialized: %llu free pages, %llu total", free_pages,
			  total_pages);
}

void *palloc_single(void)
{
	uint64_t irq = pmm_irq_save();
	spinlock_acquire(&pmm_lock);
	page_t *page = _pmm_pop();
	if (!page)
		kpanic(NULL, "pmm: out of memory");

	page->refcount = 1;
	spinlock_release(&pmm_lock);
	pmm_irq_restore(irq);

	_pmm_log_trace("pmm", "palloc_single phys=0x%llx",
				   pfndb_page_to_phys(page));
	return (void *)pfndb_page_to_phys(page);
}

page_t *palloc_page(void)
{
	uint64_t irq = pmm_irq_save();
	spinlock_acquire(&pmm_lock);
	page_t *page = _pmm_pop();
	if (!page)
		kpanic(NULL, "pmm: out of memory");

	page->refcount = 1;
	spinlock_release(&pmm_lock);
	pmm_irq_restore(irq);

	_pmm_log_trace("pmm", "palloc_page phys=0x%llx", pfndb_page_to_phys(page));
	return page;
}

void page_ref(page_t *page)
{
	uint64_t irq = pmm_irq_save();
	spinlock_acquire(&pmm_lock);
	_page_validate_used(page, "page_ref");
	page->refcount++;
	spinlock_release(&pmm_lock);
	pmm_irq_restore(irq);
	_pmm_log_trace("pmm", "page_ref phys=0x%llx refcount=%u",
				   pfndb_page_to_phys(page), page->refcount);
}

void page_unref(page_t *page)
{
	void *caller = __builtin_return_address(0);

	if (!page) {
		log_warn("pmm", "page_unref: called with NULL page");
		return;
	}

	uint64_t irq = pmm_irq_save();
	spinlock_acquire(&pmm_lock);
	if (!page_is_used(page)) {
		kpanic(NULL,
			   "page_unref: page not marked used (flags=0x%x refcount=%u sharecount=%llu page=%p phys=0x%llx caller=%p)",
			   page->flags, page->refcount, page->u2.sharecount, page,
			   pfndb_page_to_phys(page), caller);
	}
	_page_validate_used(page, "page_unref");

	if (page->refcount == 0) {
		spinlock_release(&pmm_lock);
		pmm_irq_restore(irq);
		log_warn("pmm",
				 "page_unref: double-free @ page=%p phys=0x%llx — ignoring",
				 page, pfndb_page_to_phys(page));
		return;
	}

	page->refcount--;

	_pmm_log_trace("pmm", "page_unref phys=0x%llx refcount=%u sharecount=%llu",
				   pfndb_page_to_phys(page), page->refcount,
				   page->u2.sharecount);

	if (page->refcount == 0) {
		if (page->u2.sharecount != 0) {
			kpanic(NULL,
				   "page_unref: refcount=0 but sharecount=%llu @ phys=0x%llx — "
				   "unmap all PTEs before releasing ownership",
				   page->u2.sharecount, pfndb_page_to_phys(page));
		}

		_pmm_push(page);
	}
	spinlock_release(&pmm_lock);
	pmm_irq_restore(irq);
}

void page_share(page_t *page)
{
	uint64_t irq = pmm_irq_save();
	spinlock_acquire(&pmm_lock);
	_page_validate_used(page, "page_share");

	page->u2.sharecount++;

	if (page->u2.sharecount > 1)
		page->flags |= PAGE_SHARED;
	spinlock_release(&pmm_lock);
	pmm_irq_restore(irq);

	_pmm_log_trace("pmm", "page_share phys=0x%llx sharecount=%llu",
				   pfndb_page_to_phys(page), page->u2.sharecount);
}

void page_unshare(page_t *page)
{
	uint64_t irq = pmm_irq_save();
	spinlock_acquire(&pmm_lock);
	_page_validate_used(page, "page_unshare");

	if (page->u2.sharecount == 0) {
		spinlock_release(&pmm_lock);
		pmm_irq_restore(irq);
		log_warn("pmm",
				 "page_unshare: sharecount already 0 @ page=%p phys=0x%llx "
				 "— map/unmap asymmetry (mapped with map_page_phys?)",
				 page, pfndb_page_to_phys(page));
		return;
	}

	page->u2.sharecount--;

	if (page->u2.sharecount <= 1)
		page->flags &= ~PAGE_SHARED;
	spinlock_release(&pmm_lock);
	pmm_irq_restore(irq);

	_pmm_log_trace("pmm", "page_unshare phys=0x%llx sharecount=%llu",
				   pfndb_page_to_phys(page), page->u2.sharecount);
}

void page_mark_cow(page_t *page)
{
	if (!page)
		return;

	uint64_t irq = pmm_irq_save();
	spinlock_acquire(&pmm_lock);
	_page_validate_used(page, "page_mark_cow");
	page->flags |= PAGE_COW;
	spinlock_release(&pmm_lock);
	pmm_irq_restore(irq);
}

void page_clear_cow(page_t *page)
{
	if (!page)
		return;

	uint64_t irq = pmm_irq_save();
	spinlock_acquire(&pmm_lock);
	_page_validate_used(page, "page_clear_cow");
	page->flags &= ~PAGE_COW;
	spinlock_release(&pmm_lock);
	pmm_irq_restore(irq);
}

uint64_t pmm_free_pages(void)
{
	uint64_t irq = pmm_irq_save();
	spinlock_acquire(&pmm_lock);
	uint64_t pages = free_pages;
	spinlock_release(&pmm_lock);
	pmm_irq_restore(irq);
	return pages;
}

uint64_t pmm_total_pages(void)
{
	uint64_t irq = pmm_irq_save();
	spinlock_acquire(&pmm_lock);
	uint64_t pages = total_pages;
	spinlock_release(&pmm_lock);
	pmm_irq_restore(irq);
	return pages;
}

void pmm_dump_stats(void)
{
	log_info("pmm", "free=%llu / total=%llu (%llu KiB free)", free_pages,
			 total_pages, free_pages * PAGE_SIZE / 1024);
}
