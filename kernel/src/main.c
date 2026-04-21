#include <limine.h>
#include <cpu/instr.h>
#include <dev/uart.h>
#include <util/kprintf.h>
#include <cpu/gdt.h>
#include <stdbool.h>

__attribute__((used, section(".limine_requests"))) static volatile uint64_t
	limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((
	used,
	section(
		".limine_requests"))) static volatile struct limine_framebuffer_request
	framebuffer_request = { .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
							.revision = 0 };

__attribute__((used,
			   section(".limine_requests_start"))) static volatile uint64_t
	limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t
	limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

/* lyr entry */
void lyr_entry(void)
{
	if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
		nointloop();
	}

	if (uart_init() != 0) {
		nointloop();
	}

	kprintf("Hello, World!\n");

	if (framebuffer_request.response == NULL ||
		framebuffer_request.response->framebuffer_count < 1) {
		kprintf("error: Failed to get framebuffer\n");
		nointloop();
	}

	struct limine_framebuffer *framebuffer =
		framebuffer_request.response->framebuffers[0];

	volatile uint32_t *fb = framebuffer->address;
	uint32_t w = framebuffer->width;
	uint32_t h = framebuffer->height;
	uint32_t p = framebuffer->pitch / 4;

	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			uint32_t c;

			if (y < h / 3) {
				static const uint32_t bars[8] = { 0xFF0000, 0x00FF00, 0x0000FF,
												  0xFFFF00, 0xFF00FF, 0x00FFFF,
												  0xFFFFFF, 0x000000 };
				c = bars[(x * 8) / w];
			} else if (y < (2 * h) / 3) {
				c = ((x >> 4) ^ (y >> 4)) & 1 ? 0xFFFFFF : 0x000000;
			} else {
				// Gradient
				c = ((x * 255 / w) << 16) | ((y * 255 / h) << 8) | 0x80;
			}

			fb[y * p + x] = c;
		}
	}

	kprintf("early: init GDT\n");
	gdt_init();
	kprintf("early: GDT init done\n");

	nointloop();
}