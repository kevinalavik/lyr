#ifndef _LYR_MM_PFNDB_H
#define _LYR_MM_PFNDB_H

#include <limine.h>
#include <mm/page.h>

void pfndb_init(struct limine_memmap_response *memmap);
page_t *pfndb_getdb(void);

#endif // _LYR_MM_PFNDB_H