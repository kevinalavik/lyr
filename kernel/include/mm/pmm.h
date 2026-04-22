#ifndef _LYR_MM_PMM_H
#define _LYR_MM_PMM_H

void pmm_init();
void *palloc_single();
void pfree(void *a);

#endif // _LYR_MM_PMM_H