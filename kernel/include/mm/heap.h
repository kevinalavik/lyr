#ifndef _LYR_MM_HEAP_H
#define _LYR_MM_HEAP_H

#include <stddef.h>
#include <stdint.h>

void kheap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *ptr);

void *krealloc(void *ptr, size_t new_size);

#endif // _LYR_MM_HEAP_H