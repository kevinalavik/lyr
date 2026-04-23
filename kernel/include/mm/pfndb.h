#ifndef _LYR_MM_PFNDB_H
#define _LYR_MM_PFNDB_H

#include <limine.h>
#include <mm/page.h>

void pfndb_init(struct limine_memmap_response *memmap);
page_t *pfndb_getdb(void);
uint64_t pfndb_getmax(void);
uint64_t pfndb_pfnaddr(uint64_t pfn);
page_t *pfndb_getptr(uint64_t pfn);
uint64_t pfndb_getpfn(page_t *page);
page_t *pfndb_phys_to_page(uint64_t phys);
uint64_t pfndb_page_to_phys(page_t *page);
void pfndb_dump(void);

#endif // _LYR_MM_PFNDB_H