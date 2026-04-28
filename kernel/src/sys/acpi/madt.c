#include <sys/acpi/madt.h>
#include <debug/log.h>
#include <cpu/instr.h>

madt_t *madt = NULL;

uintptr_t lapic_base = 0;

madt_lapic_t *lapics[256]; /* max of 256 cores*/
madt_ioapic_t *ioapics[256]; /* ^ */

size_t lapic_count = 0;
size_t ioapic_count = 0;

madt_iso_t *isos[16];
size_t iso_count = 0;

madt_lapic_nmi_t *nmis[224];
size_t nmi_count = 0;

static char *_madt_type_to_str(uint8_t type)
{
	switch (type) {
	case MADT_LAPIC:
		return "apic";
	case MADT_IOAPIC:
		return "ioapic";
	case MADT_ISO:
		return "interrupt source override";
	case MADT_NMI_SRC:
		return "non-maskable interrupt source";
	case MADT_LAPIC_NMI:
		return "non-maskable interrupt";
	case MADT_LAPIC_OVERRIDE:
		return "lapic address override";
	case MADT_LX2APIC:
		return "local x2apic";
	case MADT_LX2APIC_NMI:
		return "local x2apic nmi";
	default:
		return "unknown";
	}
}

void madt_init()
{
	madt = (madt_t *)acpi_find_table("APIC");
	if (!madt) {
		log_err("madt", "MADT not found");
		return;
	}
	log_debug("madt", "MADT Address: 0x%llx", (void *)madt);

	if (madt->flags & 1) {
		log_debug("madt", "Masking 8259 PIC vectors");
		outb(0x21, 0xff);
		outb(0xa1, 0xff);
	}

	lapic_base = madt->lapic_addr;

	for (uint64_t i = 0; i < (madt->hdr.length - sizeof(madt_t));) {
		madt_header_t *mhdr = (madt_header_t *)(madt->structures + i);
		switch (mhdr->type) {
		case MADT_LAPIC: {
			madt_lapic_t *lapic = (madt_lapic_t *)(madt->structures + i);
			if (lapic_count >= 256) {
				log_warn(
					"madt",
					"Reached maximum allowed CPUs, processor #%u will be left disabled.",
					lapic->id);
				break;
			}
			lapics[lapic_count++] = lapic;
			log_debug("madt",
					  "Registered LAPIC for processor #%u with _UID %u (%s)",
					  lapic->id, lapic->uid,
					  (lapic->flags & 1) ? "enabled" : "disabled");
			break;
		}
		case MADT_IOAPIC: {
			madt_ioapic_t *ioapic = (madt_ioapic_t *)(madt->structures + i);
			if (ioapic_count >= 256) {
				log_warn(
					"madt",
					"Reached maximum allowed IOAPIC controllers, IOAPIC #%u will be unused.",
					ioapic->id);
				break;
			}
			ioapics[ioapic_count++] = ioapic;
			log_debug("madt",
					  "Registered IOAPIC #%u located at 0x%llx (gsi base=%llx)",
					  ioapic->id, ioapic->addr, ioapic->gsi_base);
			break;
		}
		case MADT_ISO: {
			madt_iso_t *iso = (madt_iso_t *)(madt->structures + i);
			isos[iso_count++] = iso;
			log_trace(
				"madt",
				"Interrupt source override on bus %u with source %u (gsi=%u, flags=%x)",
				iso->bus, iso->src, iso->gsi, iso->flags);
			break;
		}
		case MADT_LAPIC_NMI: {
			madt_lapic_nmi_t *nmi = (madt_lapic_nmi_t *)(madt->structures + i);
			nmis[nmi_count++] = nmi;
			log_debug("madt",
					  "NMI for LINT#%u on processor with _UID %u, flags %x",
					  nmi->LINTn, nmi->acpi_uid, nmi->flags);
			break;
		}
		case MADT_LAPIC_OVERRIDE: {
			madt_lapic_override_t *override =
				(madt_lapic_override_t *)(madt->structures + i);
			lapic_base = override->addr;
			log_trace("madt", "Overridden LAPIC base address: 0x%llx",
					  override->addr);
			break;
		}
		case MADT_NMI_SRC:
		case MADT_LX2APIC:
		case MADT_LX2APIC_NMI:
		default:
			log_warn("madt", "Unhandled MADT Entry with type %u (%s)",
					 mhdr->type, _madt_type_to_str(mhdr->type));
			break;
		}

		i += mhdr->len;
	}

	log_debug("madt", "MADT summary: lapics=%zu ioapics=%zu isos=%zu nmis=%zu",
			  lapic_count, ioapic_count, iso_count, nmi_count);
}