#ifndef _LYR_SYS_ACPI_BGRT_H
#define _LYR_SYS_ACPI_BGRT_H

#include <stdint.h>
#include <sys/acpi.h>
#include <limine.h>

typedef struct acpi_bgrt {
	acpi_sdt_header_t header;
	uint16_t version;
	uint8_t status;
	uint8_t image_type;
	uint64_t image_address;
	uint32_t image_offset_x;
	uint32_t image_offset_y;
} __attribute__((packed)) acpi_bgrt_t;

typedef struct bmp_file_header {
	uint16_t magic;
	uint32_t file_size;
	uint16_t reserved1;
	uint16_t reserved2;
	uint32_t pixel_offset;
} __attribute__((packed)) bmp_file_header_t;

typedef struct bmp_dib_header {
	uint32_t header_size;
	int32_t width;
	int32_t height;
	uint16_t color_planes;
	uint16_t bits_per_pixel;
	uint32_t compression;
	uint32_t image_size;
	int32_t x_pixels_per_meter;
	int32_t y_pixels_per_meter;
	uint32_t colors_in_table;
	uint32_t important_colors;
} __attribute__((packed)) bmp_dib_header_t;

void bgrt_init(struct limine_framebuffer *fb);

#endif // _LYR_SYS_ACPI_BGRT_H