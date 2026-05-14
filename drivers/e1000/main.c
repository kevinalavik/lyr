#include <cpu/instr.h>
#include <dev/device.h>
#include <drv/driver.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/page.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include <net/net.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define E1000_VENDOR_INTEL 0x8086

#define E1000_REG_CTRL 0x0000
#define E1000_REG_STATUS 0x0008
#define E1000_REG_EERD 0x0014
#define E1000_REG_IMC 0x00D8
#define E1000_REG_RCTL 0x0100
#define E1000_REG_TCTL 0x0400
#define E1000_REG_TIPG 0x0410
#define E1000_REG_RDBAL 0x2800
#define E1000_REG_RDBAH 0x2804
#define E1000_REG_RDLEN 0x2808
#define E1000_REG_RDH 0x2810
#define E1000_REG_RDT 0x2818
#define E1000_REG_TDBAL 0x3800
#define E1000_REG_TDBAH 0x3804
#define E1000_REG_TDLEN 0x3808
#define E1000_REG_TDH 0x3810
#define E1000_REG_TDT 0x3818
#define E1000_REG_RAL 0x5400
#define E1000_REG_RAH 0x5404

#define E1000_STATUS_LU (1u << 1)
#define E1000_RCTL_EN (1u << 1)
#define E1000_RCTL_BAM (1u << 15)
#define E1000_RCTL_SECRC (1u << 26)
#define E1000_TCTL_EN (1u << 1)
#define E1000_TCTL_PSP (1u << 3)
#define E1000_CMD_EOP (1u << 0)
#define E1000_CMD_IFCS (1u << 1)
#define E1000_CMD_RS (1u << 3)
#define E1000_DESC_DD 0x01

#define E1000_MMIO_BASE 0xffffffffc0000000ULL
#define E1000_MMIO_STRIDE 0x20000ULL

#define RX_COUNT 128
#define TX_COUNT 128
#define RX_BUF_SIZE 2048
typedef struct __attribute__((packed)) {
	uint64_t addr;
	uint16_t length;
	uint16_t checksum;
	uint8_t status;
	uint8_t errors;
	uint16_t special;
} e1000_rx_desc_t;

typedef struct __attribute__((packed)) {
	uint64_t addr;
	uint16_t length;
	uint8_t cso;
	uint8_t cmd;
	uint8_t status;
	uint8_t css;
	uint16_t special;
} e1000_tx_desc_t;

typedef struct {
	driver_t *driver;
	volatile uint32_t *mmio;
	uint64_t mmio_phys;
	uint64_t mmio_virt;
	e1000_rx_desc_t *rx;
	e1000_tx_desc_t *tx;
	uint64_t rx_phys;
	uint64_t tx_phys;
	void *rx_buf[RX_COUNT];
	uint64_t rx_buf_phys[RX_COUNT];
	void *tx_buf[TX_COUNT];
	uint64_t tx_buf_phys[TX_COUNT];
	uint32_t rx_next;
	uint32_t tx_next;
	netdev_t *netdev;
	uint8_t mac[6];
} e1000_t;

extern uint64_t _lyr_hhdm_offset;
extern void sched_map_kernel_mmio(uint64_t virt, uint64_t phys,
								  uint64_t npages);

static uint64_t next_mmio = E1000_MMIO_BASE;
static uint32_t next_netdev_id;

static void *phys_to_virt(uint64_t phys)
{
	return (void *)(phys + _lyr_hhdm_offset);
}

static inline volatile uint32_t *e1000_mmio(e1000_t *e)
{
	if (!e || !e->mmio)
		return NULL;

	ptable_t *pt = (ptable_t *)read_cr3();
	if (pt && get_phys(pt, e->mmio_virt) != e->mmio_phys) {
		/* Keep the BAR visible in the active address space even if a process
		 * page table missed the one-time registration window. */
		map_mmio(pt, e->mmio_virt, e->mmio_phys,
				 E1000_MMIO_STRIDE / PAGE_SIZE);
	}

	return e->mmio;
}

static uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t function,
						 uint8_t off)
{
	return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
		   ((uint32_t)function << 8) | (off & 0xfc);
}

static uint32_t pci_read32(const device_pci_info_t *dev, uint8_t off)
{
	outl(PCI_CONFIG_ADDR, pci_addr(dev->bus, dev->slot, dev->function, off));
	return inl(PCI_CONFIG_DATA);
}

static void pci_write32(const device_pci_info_t *dev, uint8_t off, uint32_t v)
{
	outl(PCI_CONFIG_ADDR, pci_addr(dev->bus, dev->slot, dev->function, off));
	outl(PCI_CONFIG_DATA, v);
}

static uint16_t pci_read16(const device_pci_info_t *dev, uint8_t off)
{
	uint32_t v = pci_read32(dev, off);
	return (uint16_t)(v >> ((off & 2) * 8));
}

static void pci_write16(const device_pci_info_t *dev, uint8_t off, uint16_t v)
{
	uint32_t old = pci_read32(dev, off);
	uint32_t shift = (off & 2) * 8;
	old &= ~(0xffffu << shift);
	old |= (uint32_t)v << shift;
	pci_write32(dev, off, old);
}

static inline uint32_t erd(e1000_t *e, uint32_t reg)
{
	volatile uint32_t *mmio = e1000_mmio(e);
	return mmio ? mmio[reg / 4] : 0;
}

static inline void ewr(e1000_t *e, uint32_t reg, uint32_t val)
{
	volatile uint32_t *mmio = e1000_mmio(e);
	if (!mmio)
		return;

	mmio[reg / 4] = val;
	(void)mmio[E1000_REG_STATUS / 4];
}

static int e1000_match(const device_t *dev, void *ctx)
{
	(void)ctx;
	if (!dev || dev->bus_type != DEVICE_BUS_PCI)
		return 0;
	if (dev->pci.vendor_id != E1000_VENDOR_INTEL)
		return 0;
	if (dev->pci.class_code == 0x02 && dev->pci.subclass == 0x00)
		return 1;
	return dev->pci.device_id == 0x100e || dev->pci.device_id == 0x100f ||
		   dev->pci.device_id == 0x1004 || dev->pci.device_id == 0x1001;
}

static int e1000_send(netdev_t *dev, const void *frame, size_t len)
{
	e1000_t *e = dev->driver_data;
	if (!e || !frame || len == 0 || len > RX_BUF_SIZE)
		return -22;

	uint32_t idx = e->tx_next;
	e1000_tx_desc_t *desc = &e->tx[idx];

	uint64_t spins = 0;
	while (!(desc->status & E1000_DESC_DD) && spins++ < 1000000)
		__asm__ volatile("pause" ::: "memory");
	if (!(desc->status & E1000_DESC_DD))
		return -1;

	memcpy(e->tx_buf[idx], frame, len);
	desc->addr = e->tx_buf_phys[idx];
	desc->length = (uint16_t)len;
	desc->cso = 0;
	desc->cmd = E1000_CMD_EOP | E1000_CMD_IFCS | E1000_CMD_RS;
	desc->status = 0;
	desc->css = 0;
	desc->special = 0;

	e->tx_next = (idx + 1) % TX_COUNT;
	ewr(e, E1000_REG_TDT, e->tx_next);
	return 0;
}

static int e1000_poll(netdev_t *dev)
{
	e1000_t *e = dev->driver_data;
	if (!e)
		return -22;

	for (;;) {
		e1000_rx_desc_t *desc = &e->rx[e->rx_next];
		if (!(desc->status & E1000_DESC_DD))
			break;

		if (desc->length)
			net_receive_frame(dev, e->rx_buf[e->rx_next], desc->length);

		desc->status = 0;
		ewr(e, E1000_REG_RDT, e->rx_next);
		e->rx_next = (e->rx_next + 1) % RX_COUNT;
	}
	return 0;
}

static void e1000_read_mac(e1000_t *e)
{
	uint32_t lo = erd(e, E1000_REG_RAL);
	uint32_t hi = erd(e, E1000_REG_RAH);
	e->mac[0] = lo & 0xff;
	e->mac[1] = (lo >> 8) & 0xff;
	e->mac[2] = (lo >> 16) & 0xff;
	e->mac[3] = (lo >> 24) & 0xff;
	e->mac[4] = hi & 0xff;
	e->mac[5] = (hi >> 8) & 0xff;
}

static int e1000_alloc_rings(e1000_t *e)
{
	e->rx_phys = (uint64_t)palloc_single();
	e->tx_phys = (uint64_t)palloc_single();
	e->rx = phys_to_virt(e->rx_phys);
	e->tx = phys_to_virt(e->tx_phys);
	memset(e->rx, 0, PAGE_SIZE);
	memset(e->tx, 0, PAGE_SIZE);

	for (size_t i = 0; i < RX_COUNT; i++) {
		e->rx_buf_phys[i] = (uint64_t)palloc_single();
		e->rx_buf[i] = phys_to_virt(e->rx_buf_phys[i]);
		memset(e->rx_buf[i], 0, PAGE_SIZE);
		e->rx[i].addr = e->rx_buf_phys[i];
	}

	for (size_t i = 0; i < TX_COUNT; i++) {
		e->tx_buf_phys[i] = (uint64_t)palloc_single();
		e->tx_buf[i] = phys_to_virt(e->tx_buf_phys[i]);
		memset(e->tx_buf[i], 0, PAGE_SIZE);
		e->tx[i].addr = e->tx_buf_phys[i];
		e->tx[i].status = E1000_DESC_DD;
	}
	return 0;
}

static void e1000_hw_init(e1000_t *e)
{
	ewr(e, E1000_REG_IMC, 0xffffffffu);

	ewr(e, E1000_REG_RCTL, 0);
	ewr(e, E1000_REG_TCTL, 0);

	ewr(e, E1000_REG_RDBAL, (uint32_t)e->rx_phys);
	ewr(e, E1000_REG_RDBAH, (uint32_t)(e->rx_phys >> 32));
	ewr(e, E1000_REG_RDLEN, RX_COUNT * sizeof(e1000_rx_desc_t));
	ewr(e, E1000_REG_RDH, 0);
	ewr(e, E1000_REG_RDT, RX_COUNT - 1);
	e->rx_next = 0;

	ewr(e, E1000_REG_TDBAL, (uint32_t)e->tx_phys);
	ewr(e, E1000_REG_TDBAH, (uint32_t)(e->tx_phys >> 32));
	ewr(e, E1000_REG_TDLEN, TX_COUNT * sizeof(e1000_tx_desc_t));
	ewr(e, E1000_REG_TDH, 0);
	ewr(e, E1000_REG_TDT, 0);
	e->tx_next = 0;

	ewr(e, E1000_REG_TIPG, 10 | (8 << 10) | (6 << 20));
	ewr(e, E1000_REG_TCTL,
		E1000_TCTL_EN | E1000_TCTL_PSP | (0x10 << 4) | (0x40 << 12));
	ewr(e, E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
}

static int e1000_probe(device_t *dev, void *ctx)
{
	driver_t *driver = ctx;
	uint32_t bar0 = pci_read32(&dev->pci, 0x10);
	if (bar0 & 1) {
		driver_log(driver, "err", "e1000 I/O BAR mode is not supported");
		return -22;
	}

	uint16_t cmd = pci_read16(&dev->pci, 0x04);
	cmd |= 0x0002u | 0x0004u;
	pci_write16(&dev->pci, 0x04, cmd);

	e1000_t *e = kzalloc(sizeof(*e));
	if (!e)
		return -12;
	e->driver = driver;
	e->mmio_phys = bar0 & ~0x0full;
	e->mmio_virt = next_mmio;
	next_mmio += E1000_MMIO_STRIDE;
	sched_map_kernel_mmio(e->mmio_virt, e->mmio_phys,
						  E1000_MMIO_STRIDE / PAGE_SIZE);
	e->mmio = (volatile uint32_t *)e->mmio_virt;

	e1000_read_mac(e);
	e1000_alloc_rings(e);
	e1000_hw_init(e);

	netdev_t nd;
	memset(&nd, 0, sizeof(nd));
	npf_snprintf(nd.name, sizeof(nd.name), "eth%u", next_netdev_id++);
	memcpy(nd.mac, e->mac, sizeof(nd.mac));
	nd.link_up = (erd(e, E1000_REG_STATUS) & E1000_STATUS_LU) != 0;
	nd.mtu = NET_MTU;
	nd.send = e1000_send;
	nd.poll = e1000_poll;
	nd.driver_data = e;

	int r = netdev_register(&nd);
	if (r != 0)
		return r;

	char msg[128];
	npf_snprintf(msg, sizeof(msg),
				 "bound %s mmio=0x%llx mac=%02x:%02x:%02x:%02x:%02x:%02x",
				 dev->name, (unsigned long long)e->mmio_phys, e->mac[0],
				 e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
	driver_log(driver, "info", msg);
	return 0;
}

static int e1000_main(driver_t *driver)
{
	device_handler_t handler;
	memset(&handler, 0, sizeof(handler));
	npf_snprintf(handler.name, sizeof(handler.name), "e1000");
	handler.bus_type = DEVICE_BUS_PCI;
	handler.match = e1000_match;
	handler.probe = e1000_probe;
	handler.ctx = driver;
	return device_handler_register(&handler);
}

static const char *const e1000_imports[] = {
	"device_handler_register",
	"driver_log",
	"kzalloc",
	"memcpy",
	"memset",
	"net_receive_frame",
	"netdev_register",
	"npf_snprintf_",
	"palloc_single",
	"sched_map_kernel_mmio",
};

const driver_metadata_t lyr_driver_metadata = {
	.magic = DRIVER_MAGIC,
	.abi_version = DRIVER_ABI_VERSION,
	.name = "e1000",
	.entry = e1000_main,
	.imports = e1000_imports,
	.import_count = sizeof(e1000_imports) / sizeof(e1000_imports[0]),
};
