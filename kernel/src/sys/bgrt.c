#include <sys/bgrt.h>
#include <sys/acpi.h>
#include <debug/log.h>
#include <mm/page.h>
#include <stdbool.h>

static void blit_bmp(struct limine_framebuffer *fb, void *bmp_virt,
					 uint32_t dst_x, uint32_t dst_y)
{
	bmp_file_header_t *fhdr = (bmp_file_header_t *)bmp_virt;
	if (fhdr->magic != 0x4D42) {
		log_warn("bgrt", "BMP magic mismatch: 0x%04x", fhdr->magic);
		return;
	}

	bmp_dib_header_t *dib =
		(bmp_dib_header_t *)((uint8_t *)bmp_virt + sizeof(bmp_file_header_t));

	int32_t img_w = dib->width;
	int32_t img_h = dib->height;
	uint16_t bpp = dib->bits_per_pixel;
	bool flip = true;

	if (img_h < 0) {
		img_h = -img_h;
		flip = false;
	}

	if (bpp != 24 && bpp != 32) {
		log_warn("bgrt", "Unsupported BMP bpp: %u (need 24 or 32)", bpp);
		return;
	}

	log_info("bgrt", "BMP %dx%d bpp=%u flip=%d", img_w, img_h, bpp, flip);

	uint8_t *pixels = (uint8_t *)bmp_virt + fhdr->pixel_offset;
	uint32_t bytes_pp = bpp / 8;
	uint32_t row_stride = (img_w * bytes_pp + 3) & ~3u;

	uint32_t fb_w = (uint32_t)fb->width;
	uint32_t fb_h = (uint32_t)fb->height;

	for (int32_t row = 0; row < img_h; row++) {
		int32_t src_row = flip ? (img_h - 1 - row) : row;
		uint8_t *src = pixels + (uint32_t)src_row * row_stride;

		uint32_t dy = dst_y + (uint32_t)row;
		if (dy >= fb_h)
			break;

		for (int32_t col = 0; col < img_w; col++) {
			uint32_t dx = dst_x + (uint32_t)col;
			if (dx >= fb_w)
				break;

			uint8_t b = src[col * bytes_pp + 0];
			uint8_t g = src[col * bytes_pp + 1];
			uint8_t r = src[col * bytes_pp + 2];

			uint32_t colour = ((uint32_t)r << fb->red_mask_shift) |
							  ((uint32_t)g << fb->green_mask_shift) |
							  ((uint32_t)b << fb->blue_mask_shift);

			uint32_t *pixel_ptr =
				(uint32_t *)((uint8_t *)fb->address + dy * fb->pitch + dx * 4);
			*pixel_ptr = colour;
		}
	}
}

void bgrt_init(struct limine_framebuffer *fb)
{
	acpi_bgrt_t *bgrt = (acpi_bgrt_t *)acpi_find_table("BGRT");
	if (bgrt == NULL) {
		log_warn("bgrt", "BGRT table not found");
		return;
	}

	log_info("bgrt", "version=%u status=0x%02x image_type=%u", bgrt->version,
			 bgrt->status, bgrt->image_type);
	log_info("bgrt", "image @ phys 0x%llx  offset (%u, %u)",
			 bgrt->image_address, bgrt->image_offset_x, bgrt->image_offset_y);

	if ((bgrt->status & 0x1) == 0) {
		log_warn("bgrt", "BGRT status says image is not valid, skipping");
		return;
	}

	if (bgrt->image_type != 0) {
		log_warn("bgrt", "Unknown BGRT image_type %u (only BMP=0 supported)",
				 bgrt->image_type);
		return;
	}

	void *bmp = PHYS_TO_VIRT(bgrt->image_address);
	blit_bmp(fb, bmp, bgrt->image_offset_x, bgrt->image_offset_y);
	log_info("bgrt", "boot logo drawn at (%u, %u)", bgrt->image_offset_x,
			 bgrt->image_offset_y);
}