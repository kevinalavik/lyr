#ifndef _LYR_CPU_GDT_H
#define _LYR_CPU_GDT_H

#include <stdint.h>

typedef struct {
	uint16_t limit_low;
	uint16_t base_low;
	uint8_t base_mid;
	uint8_t access;
	uint8_t limit_flags;
	uint8_t base_high;
} __attribute__((packed)) gdt_descriptor_t;

typedef struct {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed)) gdtr_t;

typedef struct {
	gdt_descriptor_t entries[5];
} __attribute__((packed)) gdt_t;

void gdt_init();

#endif // _LYR_CPU_GDT_H