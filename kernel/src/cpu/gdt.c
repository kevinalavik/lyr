#include <cpu/gdt.h>
/*
Access Byte
-----------
   bit 7 : Present
   bits 6-5 : DPL
   bit 4 : Descriptor type (1 = code/data)
   bit 3 : Executable (1 = code)
   bit 2 : Direction/Conforming
   bit 1 : Read/Write
   bit 0 : Accessed
*/

#define GDT_P 0b10000000
#define GDT_DPL0 0b00000000
#define GDT_DPL3 0b01100000
#define GDT_S 0b00010000

#define GDT_CODE 0b00001000
#define GDT_DATA 0b00000000

#define GDT_RW 0b00000010
#define GDT_A 0b00000001

#define GDT_KERNEL_CODE (GDT_P | GDT_DPL0 | GDT_S | GDT_CODE | GDT_RW)
#define GDT_KERNEL_DATA (GDT_P | GDT_DPL0 | GDT_S | GDT_DATA | GDT_RW)

#define GDT_USER_CODE (GDT_P | GDT_DPL3 | GDT_S | GDT_CODE | GDT_RW)
#define GDT_USER_DATA (GDT_P | GDT_DPL3 | GDT_S | GDT_DATA | GDT_RW)

#define GDT_FLAG_GRAN_4K 0b10000000
#define GDT_FLAG_64BIT 0b00100000

#define _ENTRY(base, limit, access, flags)                         \
	((gdt_descriptor_t){                                           \
		(uint16_t)((limit) & 0xFFFF), (uint16_t)((base) & 0xFFFF), \
		(uint8_t)(((base) >> 16) & 0xFF), (uint8_t)(access),       \
		(uint8_t)((((limit) >> 16) & 0x0F) | ((flags) & 0xF0)),    \
		(uint8_t)(((base) >> 24) & 0xFF) })

gdt_t gdt = { .entries = {
				  _ENTRY(0, 0, 0, 0),

				  /* kernel */
				  _ENTRY(0, 0xFFFF, GDT_KERNEL_CODE,
						 GDT_FLAG_GRAN_4K | GDT_FLAG_64BIT),

				  _ENTRY(0, 0xFFFF, GDT_KERNEL_DATA, GDT_FLAG_GRAN_4K),

				  /* user */
				  _ENTRY(0, 0xFFFF, GDT_USER_CODE,
						 GDT_FLAG_GRAN_4K | GDT_FLAG_64BIT),

				  _ENTRY(0, 0xFFFF, GDT_USER_DATA, GDT_FLAG_GRAN_4K),
			  } };

gdtr_t gdtr = { .limit = sizeof(gdt.entries) - 1,
				.base = (uint64_t)&gdt.entries };

void gdt_init()
{
	__asm__ volatile("lgdt %[g]\n"
					 "pushq $0x08\n"
					 "lea 1f(%%rip), %%rax\n"
					 "pushq %%rax\n"
					 "lretq\n"
					 "1:\n"
					 "mov $0x10, %%ax\n"
					 "mov %%ax, %%ds\n"
					 "mov %%ax, %%es\n"
					 "mov %%ax, %%ss\n"
					 "mov %%ax, %%fs\n"
					 "mov %%ax, %%gs\n"
					 :
					 : [g] "m"(gdtr)
					 : "rax", "memory");
}