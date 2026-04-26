#ifndef _LYR_SYS_ACPI_H
#define _LYR_SYS_ACPI_H

#include <stdint.h>
#include <stddef.h>
#include <lib/string.h>

#define ACPI_SIG_RSDP "RSD PTR "

typedef struct acpi_rsdp {
	char signature[8];
	uint8_t checksum;
	char oem_id[6];
	uint8_t revision;
	uint32_t rsdt_address;
} __attribute__((packed)) acpi_rsdp_t;

/* ACPI 2.0+ extension */
typedef struct acpi_xsdp {
	acpi_rsdp_t v1;
	uint32_t length;
	uint64_t xsdt_address;
	uint8_t extended_checksum;
	uint8_t reserved[3];
} __attribute__((packed)) acpi_xsdp_t;

typedef struct acpi_sdt_header {
	char signature[4];
	uint32_t length;
	uint8_t revision;
	uint8_t checksum;
	char oem_id[6];
	char oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

/* Generic SDTs */
typedef struct acpi_rsdt {
	acpi_sdt_header_t header;
	uint32_t tables[];
} __attribute__((packed)) acpi_rsdt_t;

typedef struct acpi_xsdt {
	acpi_sdt_header_t header;
	uint64_t tables[];
} __attribute__((packed)) acpi_xsdt_t;

void acpi_init(void *rsdp);
acpi_sdt_header_t *acpi_find_table(const char *sig);
void acpi_dump_tables(void);

#endif // _LYR_SYS_ACPI_H