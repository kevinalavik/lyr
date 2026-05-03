#include <sys/apic.h>
#include <cpu/instr.h>
#include <dev/async.h>
#include <mm/vmm.h>
#include <sys/acpi/madt.h>
#include <stdint.h>
#include <stdbool.h>
#include <debug/log.h>
#include <sys/smp.h>
#include <cpu/idt.h>

#define IOREGSEL 0x00
#define IOWIN 0x10

/*
 * Spurious interrupt vector programmed into SVR.
 * Must match APIC_SPURIOUS_VECTOR in idt.c.
 * Bits [3:0] of the spurious vector must be 0b1111 (i.e. the low nibble
 * must be 0xF) on some older CPUs.  0xFF satisfies this requirement.
 */
#define SPURIOUS_VECTOR 0xFF

extern uintptr_t lapic_base;
extern madt_lapic_t *lapics[];
extern madt_ioapic_t *ioapics[];
extern size_t lapic_count;
extern size_t ioapic_count;
extern madt_iso_t *isos[16];
extern size_t iso_count;
extern madt_lapic_nmi_t *nmis[224];
extern size_t nmi_count;

uint64_t apic_msr_read(uint64_t offset)
{
	return rdmsr(APIC_BASE_MSR + offset);
}

void apic_msr_write(uint64_t offset, uint64_t val)
{
	wrmsr(APIC_BASE_MSR + offset, val);
}

uint32_t ioapic_read(uintptr_t base, uint8_t regoff)
{
	async_io_mmio_write32_sync(base + IOREGSEL, regoff);
	return async_io_mmio_read32_sync(base + IOWIN);
}

void ioapic_write(uintptr_t base, uint8_t regoff, uint32_t data)
{
	async_io_mmio_write32_sync(base + IOREGSEL, regoff);
	async_io_mmio_write32_sync(base + IOWIN, data);
}

uint32_t lapic_read(uint16_t reg)
{
	return async_io_mmio_read32_sync(lapic_base + reg);
}

void lapic_write(uint16_t reg, uint32_t val)
{
	async_io_mmio_write32_sync(lapic_base + reg, val);
}

void apic_send_eoi(void)
{
	lapic_write(APIC_EOI, 0);
}

/*
 * ioapic_write_red - program one IOAPIC redirection entry.
 *
 * Uses PHYSICAL destination mode: dest = LAPIC hardware ID.
 * This guarantees the interrupt goes to exactly one CPU.
 * Logical mode with dest=0xFF would broadcast to all CPUs — never use that.
 */
void ioapic_write_red(uint32_t gsi, uint8_t vec, uint8_t delivery_mode,
					  uint8_t polarity, uint8_t trigger_mode, uint8_t dest)
{
	union ioapic_redirect_entry redent = { 0 };
	redent.vec = vec;
	redent.delivery_mode = delivery_mode;
	redent.destination_mode = IOAPIC_PHYSICAL_DESTINATION;
	redent.delivery_status = 0;
	redent.pin_polarity = polarity;
	redent.remote_irr = 0;
	redent.trigger_mode = trigger_mode;
	redent.mask = 0;
	redent.reserved = 0;
	redent.dest = dest;

	/* Apply MADT interrupt source overrides. */
	for (size_t i = 0; i < iso_count; i++) {
		if ((vec - IRQ_BASE) == isos[i]->src) {
			gsi = isos[i]->gsi;
			if (isos[i]->flags & 2)
				redent.pin_polarity = IOAPIC_ACTIVE_LO;
			if (isos[i]->flags & 8)
				redent.trigger_mode = IOAPIC_TRIGGER_LEVEL;
			break;
		}
	}

	/* Find the IOAPIC that owns this GSI. */
	size_t i;
	bool found = false;
	for (i = 0; i < ioapic_count; i++) {
		uint8_t maxreds =
			(ioapic_read((uint64_t)PHYS_TO_VIRT(ioapics[i]->addr), IOAPICVER) >>
			 16) &
			0xFF;
		if (ioapics[i]->gsi_base <= gsi &&
			ioapics[i]->gsi_base + maxreds > gsi) {
			found = true;
			break;
		}
	}

	if (!found) {
		log_err("apic", "No IOAPIC found for GSI %u", gsi);
		return;
	}

	uint32_t pin = gsi - ioapics[i]->gsi_base;
	uintptr_t base = (uint64_t)PHYS_TO_VIRT(ioapics[i]->addr);
	ioapic_write(base, IOAPICREDTBLL(pin), redent.bytes.low);
	ioapic_write(base, IOAPICREDTBLH(pin), redent.bytes.high);

	log_trace("apic", "IOAPIC redir: vec=%u gsi=%u dest_lapic=%u mode=phys",
			  vec, gsi, dest);
}

/*
 * apic_cpu_init - initialise the local APIC of the calling CPU.
 *
 * @cpu_index: sequential 0-based CPU index (0=BSP, 1..N=APs).
 *
 * Register map (Intel SDM Vol 3A Table 10-1):
 *   0x020 = LAPIC ID
 *   0x080 = Task Priority Register (TPR)
 *   0x0B0 = EOI
 *   0x0D0 = Logical Destination Register (LDR)
 *   0x0E0 = Destination Format Register (DFR)
 *   0x0F0 = Spurious Interrupt Vector Register (SVR)
 *   0x320 = LVT Timer
 *   0x330 = LVT Thermal Monitor
 *   0x340 = LVT Performance Counter
 *   0x350 = LVT LINT0
 *   0x360 = LVT LINT1
 *   0x370 = LVT Error
 */
void apic_cpu_init(uint8_t cpu_index)
{
	/* Enable xAPIC via IA32_APIC_BASE MSR.  Clear x2APIC enable bit (10)
     * and set global enable bit (11). */
	uint64_t base_msr = rdmsr(0x1B);
	wrmsr(0x1B, (base_msr | (1ULL << 11)) & ~(1ULL << 10));

	/*
     * Spurious Interrupt Vector Register:
     *   Bit  8   = APIC software enable
     *   Bits 7:0 = spurious vector (must have low nibble = 0xF on older HW)
     * Write the full value; do not read-modify-write to avoid stale state.
     */
	lapic_write(APIC_SPURIOUS_IVR, (1 << 8) | SPURIOUS_VECTOR);

	/*
     * Destination Format Register: flat model.
     * Logical Destination Register: one bit per CPU.
     * We use physical destination mode for all actual routing, but
     * setting these correctly prevents confusion if anything reads them.
     */
	lapic_write(APIC_DEST_FORMAT, 0xFFFFFFFF);
	if (cpu_index < 8)
		lapic_write(APIC_LOCAL_DEST, (uint32_t)(1u << cpu_index) << 24);
	else
		lapic_write(APIC_LOCAL_DEST, 0);

	/*
     * Mask all LVT entries.  Bit 16 = mask.
     * Correct register offsets per Intel SDM:
     */
	lapic_write(0x320, 0x10000); /* LVT Timer            — masked */
	lapic_write(0x330, 0x10000); /* LVT Thermal Monitor  — masked */
	lapic_write(0x340, 0x10000); /* LVT Perf Counter     — masked */
	lapic_write(0x350, 0x10000); /* LVT LINT0            — masked */
	lapic_write(0x360, 0x10000); /* LVT LINT1            — masked */
	lapic_write(0x370, 0x10000); /* LVT Error            — masked */

	/* Task Priority Register = 0: accept interrupts of all priorities. */
	lapic_write(APIC_TPR, 0);

	/* Clear any stale APIC errors by doing a write to ESR then reading it. */
	lapic_write(APIC_ERROR_STATUS, 0);
	(void)lapic_read(APIC_ERROR_STATUS);
}

void apic_timer_init(uint32_t hz)
{
	if (!hz)
		hz = 100;

	lapic_write(APIC_TIMER_DIVIDE, 0x3); /* divide by 16 */
	lapic_write(APIC_LVT_TIMER, APIC_TIMER_VECTOR | (1u << 17)); /* periodic */
	lapic_write(APIC_TIMER_INITCNT, 100000000u / hz);

	log_debug("apic", "CPU %u LAPIC timer enabled at ~%uHz",
			  get_cpu_local()->cpu_index, hz);
}

void apic_init(void)
{
	map_page_phys(_lyr_kernel_vas->pml4, (uint64_t)PHYS_TO_VIRT(lapic_base),
				  lapic_base, VMM_PRESENT | VMM_WRITABLE);
	lapic_base = (uint64_t)PHYS_TO_VIRT(lapic_base);
	apic_cpu_init(0);

	for (size_t i = 0; i < ioapic_count; i++) {
		log_trace("apic", "Mapping I/O APIC #%zu (phys 0x%llx -> virt 0x%llx)",
				  i, ioapics[i]->addr,
				  (uint64_t)PHYS_TO_VIRT(ioapics[i]->addr));
		map_page_phys(_lyr_kernel_vas->pml4,
					  (uint64_t)PHYS_TO_VIRT(ioapics[i]->addr),
					  ioapics[i]->addr, VMM_PRESENT | VMM_WRITABLE);

		uint8_t maxreds =
			(ioapic_read((uint64_t)PHYS_TO_VIRT(ioapics[i]->addr), IOAPICVER) >>
			 16) &
			0xFF;
		log_debug("apic", "Masking %u redirection entries for I/O APIC #%zu",
				  maxreds, i);

		for (int n = 0; n < maxreds; n++) {
			ioapic_write((uint64_t)PHYS_TO_VIRT(ioapics[i]->addr),
						 IOAPICREDTBLL(n), 0x10000); /* masked */
			ioapic_write((uint64_t)PHYS_TO_VIRT(ioapics[i]->addr),
						 IOAPICREDTBLH(n), 0);
		}
	}
}
