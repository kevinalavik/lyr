#include <limine.h>
#include <cpu/instr.h>
#include <dev/uart.h>
#include <util/kprintf.h>
#include <cpu/gdt.h>
#include <stdbool.h>
#include <lib/lyrterm.h>
#include <debug/log.h>
#include <debug/assert.h>

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

	assert(framebuffer_request.response != NULL);
	assert(framebuffer_request.response->framebuffer_count >= 1);
	assert(framebuffer_request.response->framebuffers[0] != NULL);
	struct limine_framebuffer *framebuffer =
		framebuffer_request.response->framebuffers[0];

	lyrterm_init(framebuffer->address, framebuffer->width, framebuffer->height,
				 framebuffer->pitch / 4);
	ok("lyrterm OK");

	gdt_init();
	ok("gdt OK");

	assert(NULL != NULL);

	nointloop();
}