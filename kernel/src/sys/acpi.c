#include <sys/acpi.h>
#include <mm/vmm.h>
#include <debug/assert.h>
#include <debug/log.h>
#include <debug/panic.h>

static int g_xsdt = 0;
static acpi_sdt_header_t *g_root = NULL;

#define ACPI_RSDP_VALID(rsdp) (memcmp((rsdp)->signature, ACPI_SIG_RSDP, 8) == 0)
#define ACPI_VIRT(p) ((void *)PHYS_TO_VIRT((uintptr_t)(p)))
#define ACPI_SDT(ptr) ((acpi_sdt_header_t *)(ptr))

static acpi_sdt_header_t *_acpi_check_table(uint64_t addr, const char *sig)
{
	acpi_sdt_header_t *hdr = ACPI_VIRT(addr);

	if (memcmp(hdr->signature, sig, 4) == 0) {
		return hdr;
	}

	return NULL;
}

acpi_sdt_header_t *acpi_find_table(const char *sig)
{
	if (!g_root || !sig)
		return NULL;

	acpi_sdt_header_t *hdr = g_root;

	uint32_t entries =
		(hdr->length - sizeof(acpi_sdt_header_t)) / (g_xsdt ? 8 : 4);

	if (g_xsdt) {
		acpi_xsdt_t *xsdt = (acpi_xsdt_t *)hdr;

		for (uint32_t i = 0; i < entries; i++) {
			uint64_t phys = xsdt->tables[i];

			acpi_sdt_header_t *t = _acpi_check_table(phys, sig);

			if (t)
				return t;
		}

		return NULL;
	}

	acpi_rsdt_t *rsdt = (acpi_rsdt_t *)hdr;

	for (uint32_t i = 0; i < entries; i++) {
		uint32_t phys = rsdt->tables[i];

		acpi_sdt_header_t *t = _acpi_check_table(phys, sig);

		if (t)
			return t;
	}

	return NULL;
}

void acpi_dump_tables(void)
{
	acpi_sdt_header_t *hdr = g_root;

	uint32_t entries =
		(hdr->length - sizeof(acpi_sdt_header_t)) / (g_xsdt ? 8 : 4);

	if (g_xsdt) {
		acpi_xsdt_t *xsdt = (acpi_xsdt_t *)hdr;

		for (uint32_t i = 0; i < entries; i++) {
			acpi_sdt_header_t *t = ACPI_SDT(ACPI_VIRT(xsdt->tables[i]));

			log_info("acpi", "table[%u]: %.4s at %p", i, t->signature, t);
		}
	} else {
		acpi_rsdt_t *rsdt = (acpi_rsdt_t *)hdr;

		for (uint32_t i = 0; i < entries; i++) {
			acpi_sdt_header_t *t = ACPI_SDT(ACPI_VIRT(rsdt->tables[i]));

			log_info("acpi", "table[%u]: %.4s at %p", i, t->signature, t);
		}
	}
}

void acpi_init(void *rsdp)
{
	assert(rsdp);

	acpi_rsdp_t *r = (acpi_rsdp_t *)rsdp;

	if (!ACPI_RSDP_VALID(r)) {
		kpanic(NULL, "Invalid RSDP signature");
	}

	if (r->revision < 2) {
		g_xsdt = 0;
		g_root = ACPI_SDT(ACPI_VIRT(r->rsdt_address));

		log_debug("acpi", "Using RSDT at %p", g_root);
		return;
	}

	acpi_xsdp_t *x = (acpi_xsdp_t *)r;

	if (x->xsdt_address) {
		g_xsdt = 1;
		g_root = ACPI_SDT(ACPI_VIRT(x->xsdt_address));

		log_debug("acpi", "Using XSDT at %p", g_root);
		return;
	}

	g_xsdt = 0;
	g_root = ACPI_SDT(ACPI_VIRT(r->rsdt_address));

	log_warn("acpi", "XSDT missing, fallback to RSDT at %p", g_root);
}