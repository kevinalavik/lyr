#include <mm/pmm.h>
#include <mm/pfndb.h>
#include <debug/log.h>
#include <debug/panic.h>
#include <mm/page.h>
#include <debug/assert.h>

static page_t *freelist = NULL;

/* freelist utils for pop/push */
static page_t *_pmm_pop(void)
{
	if (!freelist)
		return NULL;

	page_t *page = freelist;
	freelist = page->u1.next;

	page->u1.next = NULL;

	/* mark the pfn as used */
	assert(page->flags & PAGE_FREE);
	page->flags &= ~PAGE_FREE;
	page->flags |= PAGE_USED;

	return page;
}

static void _pmm_push(page_t *page)
{
	assert(page);
	assert(!(
		page->flags &
		PAGE_FREE)); // assert on double free, prob should panic instead to handle in userspace

	page->flags &= ~PAGE_USED;
	page->flags |= PAGE_FREE;

	page->u1.next = freelist;
	freelist = page;
}

/* main lyr api */
void pmm_init(void)
{
	page_t *pfndb = pfndb_getdb();
	assert(pfndb);

	uint64_t max_pfn = pfndb_getmax();

	freelist = NULL;

	for (uint64_t i = 0; i <= max_pfn; i++) {
		page_t *page = pfndb_getptr(i);

		if (!(page->flags & PAGE_FREE))
			continue;

		page->u1.next = freelist;
		freelist = page;
	}
}

void *palloc_single()
{
	page_t *p = _pmm_pop();
	if (!p)
		kpanic(NULL, "pmm: out of memory");

	return (void *)pfndb_page_to_phys(p);
}

void pfree(void *a)
{
	if (!a)
		kpanic(NULL, "pmm: attempted to free NULL address");
	page_t *p = pfndb_phys_to_page((uint64_t)a);
	_pmm_push(p);
}