#ifndef _LYR_MM_PMM_H
#define _LYR_MM_PMM_H

#include <stdint.h>
#include <mm/page.h>

#define _PMM_TRACE 1

void pmm_init(void);

void *palloc_single(void);
page_t *palloc_page(void);

void page_ref(page_t *page);
void page_unref(page_t *page);

void page_share(page_t *page);
void page_unshare(page_t *page);
void page_mark_cow(page_t *page);
void page_clear_cow(page_t *page);

uint64_t pmm_free_pages(void);
uint64_t pmm_total_pages(void);
void pmm_dump_stats(void);

#endif // _LYR_MM_PMM_H
