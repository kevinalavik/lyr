#include <boot/axboot.h>
#include <cpu/instr.h>
#include <dev/uart.h>
#include <util/kprintf.h>
#include <cpu/gdt.h>

void lyr_entry(struct aurix_parameters *params)
{
	if (uart_init() != 0) {
		nointloop();
	}

	kprintf("Hello, World!\n");
	struct aurix_framebuffer framebuffer = params->framebuffer;

	volatile uint32_t *fb_ptr = (uint32_t *)framebuffer.addr;
	for (size_t y = 0; y < framebuffer.height; y++) {
		for (size_t x = 0; x < framebuffer.width; x++) {
			uint32_t nX = x * 255 / framebuffer.width;
			uint32_t nY = y * 255 / framebuffer.height;
			fb_ptr[y * (framebuffer.pitch / 4) + x] = (nY << 8) | nX;
		}
	}

	kprintf("early: init GDT\n");
	gdt_init();
	kprintf("early: GDT init done\n");

	nointloop();
}