#include <cpu/instr.h>
#include <dev/block.h>
#include <dev/device.h>
#include <drv/driver.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/page.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include <sync/spinlock.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define NVME_REG_CAP 0x0000
#define NVME_REG_CC 0x0014
#define NVME_REG_CSTS 0x001C
#define NVME_REG_AQA 0x0024
#define NVME_REG_ASQ 0x0028
#define NVME_REG_ACQ 0x0030
#define NVME_REG_DBS 0x1000

#define NVME_CC_EN (1u << 0)
#define NVME_CC_IOSQES_64 (6u << 16)
#define NVME_CC_IOCQES_16 (4u << 20)
#define NVME_CSTS_RDY (1u << 0)
#define NVME_CSTS_CFS (1u << 1)

#define NVME_ADMIN_CREATE_SQ 0x01
#define NVME_ADMIN_CREATE_CQ 0x05
#define NVME_ADMIN_IDENTIFY 0x06
#define NVME_IO_WRITE 0x01
#define NVME_IO_READ 0x02

#define NVME_QUEUE_DEPTH 64
#define NVME_ADMIN_QID 0
#define NVME_IO_QID 1
#define NVME_MMIO_BASE 0xffffffffb0000000ULL
#define NVME_MMIO_STRIDE 0x20000ULL

typedef struct {
	uint32_t cdw0;
	uint32_t nsid;
	uint64_t rsvd2;
	uint64_t mptr;
	uint64_t prp1;
	uint64_t prp2;
	uint32_t cdw10;
	uint32_t cdw11;
	uint32_t cdw12;
	uint32_t cdw13;
	uint32_t cdw14;
	uint32_t cdw15;
} __attribute__((packed)) nvme_cmd_t;

typedef struct {
	uint32_t result;
	uint32_t rsvd;
	uint16_t sq_head;
	uint16_t sq_id;
	uint16_t cid;
	uint16_t status;
} __attribute__((packed)) nvme_cqe_t;

typedef struct {
	driver_t *driver;
	volatile uint8_t *regs;
	uint64_t mmio_virt;
	uint32_t db_stride;
	uint32_t queue_depth;
	nvme_cmd_t *asq;
	nvme_cqe_t *acq;
	uint64_t asq_phys;
	uint64_t acq_phys;
	uint16_t admin_sq_tail;
	uint16_t admin_cq_head;
	uint8_t admin_cq_phase;
	nvme_cmd_t *iosq;
	nvme_cqe_t *iocq;
	uint64_t iosq_phys;
	uint64_t iocq_phys;
	uint16_t io_sq_tail;
	uint16_t io_cq_head;
	uint8_t io_cq_phase;
	uint16_t next_cid;
	spinlock_t io_lock;
	void *dma;
	uint64_t dma_phys;
} nvme_t;

typedef struct {
	nvme_t *ctrl;
	uint32_t nsid;
	uint32_t lba_size;
	uint64_t block_count;
	char name[BLOCK_NAME_MAX + 1];
} nvme_ns_t;

extern ptable_t *kernel_ptable;
extern uint64_t _lyr_hhdm_offset;

static uint64_t next_mmio = NVME_MMIO_BASE;
static uint32_t next_ctrl_id;

static void *phys_to_virt(uint64_t phys)
{
	return (void *)(phys + _lyr_hhdm_offset);
}

static uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t function,
						 uint8_t off)
{
	return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
		   ((uint32_t)function << 8) | (off & 0xFC);
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
	old &= ~(0xFFFFu << shift);
	old |= (uint32_t)v << shift;
	pci_write32(dev, off, old);
}

static uint32_t nrd32(nvme_t *n, uint32_t off)
{
	return *(volatile uint32_t *)(n->regs + off);
}

static uint64_t nrd64(nvme_t *n, uint32_t off)
{
	uint64_t lo = *(volatile uint32_t *)(n->regs + off);
	uint64_t hi = *(volatile uint32_t *)(n->regs + off + 4);
	return lo | (hi << 32);
}

static void nwr32(nvme_t *n, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(n->regs + off) = v;
}

static void nwr64(nvme_t *n, uint32_t off, uint64_t v)
{
	*(volatile uint32_t *)(n->regs + off) = (uint32_t)v;
	*(volatile uint32_t *)(n->regs + off + 4) = (uint32_t)(v >> 32);
}

static uint32_t sq_db(nvme_t *n, uint16_t qid)
{
	return NVME_REG_DBS + (uint32_t)(2 * qid) * n->db_stride;
}

static uint32_t cq_db(nvme_t *n, uint16_t qid)
{
	return NVME_REG_DBS + (uint32_t)(2 * qid + 1) * n->db_stride;
}

static void *dma_page(uint64_t *phys)
{
	*phys = (uint64_t)palloc_single();
	void *virt = phys_to_virt(*phys);
	memset(virt, 0, PAGE_SIZE);
	return virt;
}

static int wait_ready(nvme_t *n, int ready)
{
	for (uint64_t i = 0; i < 100000000ULL; i++) {
		uint32_t csts = nrd32(n, NVME_REG_CSTS);
		if (csts & NVME_CSTS_CFS)
			return VFS_ERR_INVAL;
		if (((csts & NVME_CSTS_RDY) != 0) == ready)
			return VFS_OK;
		__asm__ volatile("pause" ::: "memory");
	}
	return VFS_ERR_TIMEOUT;
}

static int submit_wait(nvme_t *n, uint16_t qid, nvme_cmd_t *cmd,
					   uint32_t *result)
{
	uint16_t cid = n->next_cid++;
	if (n->next_cid == 0)
		n->next_cid = 1;
	cmd->cdw0 |= (uint32_t)cid << 16;

	nvme_cmd_t *sq = qid == NVME_ADMIN_QID ? n->asq : n->iosq;
	nvme_cqe_t *cq = qid == NVME_ADMIN_QID ? n->acq : n->iocq;
	uint16_t *sq_tail =
		qid == NVME_ADMIN_QID ? &n->admin_sq_tail : &n->io_sq_tail;
	uint16_t *cq_head =
		qid == NVME_ADMIN_QID ? &n->admin_cq_head : &n->io_cq_head;
	uint8_t *phase =
		qid == NVME_ADMIN_QID ? &n->admin_cq_phase : &n->io_cq_phase;

	memcpy(&sq[*sq_tail], cmd, sizeof(*cmd));
	*sq_tail = (uint16_t)((*sq_tail + 1) % n->queue_depth);
	nwr32(n, sq_db(n, qid), *sq_tail);

	for (uint64_t i = 0; i < 100000000ULL; i++) {
		nvme_cqe_t *entry = &cq[*cq_head];
		if (((entry->status & 1) == *phase) && entry->cid == cid) {
			uint16_t status = entry->status >> 1;
			if (result)
				*result = entry->result;
			*cq_head = (uint16_t)((*cq_head + 1) % n->queue_depth);
			if (*cq_head == 0)
				*phase ^= 1;
			nwr32(n, cq_db(n, qid), *cq_head);
			return status == 0 ? VFS_OK : VFS_ERR_INVAL;
		}
		__asm__ volatile("pause" ::: "memory");
	}
	return VFS_ERR_TIMEOUT;
}

static int identify(nvme_t *n, uint32_t nsid, uint32_t cns)
{
	memset(n->dma, 0, PAGE_SIZE);
	nvme_cmd_t cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.cdw0 = NVME_ADMIN_IDENTIFY;
	cmd.nsid = nsid;
	cmd.prp1 = n->dma_phys;
	cmd.cdw10 = cns;
	return submit_wait(n, NVME_ADMIN_QID, &cmd, NULL);
}

static int create_io_queues(nvme_t *n)
{
	n->iocq = dma_page(&n->iocq_phys);
	n->iosq = dma_page(&n->iosq_phys);
	n->io_cq_phase = 1;

	nvme_cmd_t cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.cdw0 = NVME_ADMIN_CREATE_CQ;
	cmd.prp1 = n->iocq_phys;
	cmd.cdw10 = ((n->queue_depth - 1) << 16) | NVME_IO_QID;
	cmd.cdw11 = 1;
	int r = submit_wait(n, NVME_ADMIN_QID, &cmd, NULL);
	if (r != VFS_OK)
		return r;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cdw0 = NVME_ADMIN_CREATE_SQ;
	cmd.prp1 = n->iosq_phys;
	cmd.cdw10 = ((n->queue_depth - 1) << 16) | NVME_IO_QID;
	cmd.cdw11 = (NVME_IO_QID << 16) | 1;
	return submit_wait(n, NVME_ADMIN_QID, &cmd, NULL);
}

static uint64_t rdle64(const uint8_t *p)
{
	uint64_t v = 0;
	for (size_t i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (i * 8);
	return v;
}

static int nvme_read_blocks(block_device_t *bdev, uint64_t lba, uint32_t count,
							void *buf);
static int nvme_write_blocks(block_device_t *bdev, uint64_t lba, uint32_t count,
							 const void *buf);

static uint32_t identify_controller_namespace_count(nvme_t *n)
{
	if (identify(n, 0, 1) != VFS_OK)
		return 0;
	uint8_t *id = n->dma;
	return (uint32_t)id[516] | ((uint32_t)id[517] << 8) |
		   ((uint32_t)id[518] << 16) | ((uint32_t)id[519] << 24);
}

static int init_namespace(nvme_t *n, nvme_ns_t *ns, uint32_t ctrl_id,
						  uint32_t nsid)
{
	int r = identify(n, nsid, 0);
	if (r != VFS_OK)
		return r;

	uint8_t *id = n->dma;
	ns->ctrl = n;
	ns->nsid = nsid;
	ns->block_count = rdle64(id);
	uint8_t flbas = id[26] & 0x0F;
	uint8_t lbads = id[128 + flbas * 4 + 2];
	ns->lba_size = 1u << lbads;
	npf_snprintf(ns->name, sizeof(ns->name), "nvme%un%u", ctrl_id, nsid);
	if (ns->lba_size < 512 || ns->lba_size > 4096 || ns->block_count == 0)
		return VFS_ERR_INVAL;
	return VFS_OK;
}

static int register_namespace(driver_t *driver, nvme_t *n, uint32_t ctrl_id,
							  uint32_t nsid)
{
	nvme_ns_t *ns = kzalloc(sizeof(*ns));
	if (!ns)
		return VFS_ERR_NOMEM;

	int r = init_namespace(n, ns, ctrl_id, nsid);
	if (r != VFS_OK) {
		kfree(ns);
		return r;
	}

	block_device_t bdev;
	memset(&bdev, 0, sizeof(bdev));
	memcpy(bdev.name, ns->name, strlen(ns->name));
	bdev.block_size = ns->lba_size;
	bdev.block_count = ns->block_count;
	bdev.read_blocks = nvme_read_blocks;
	bdev.write_blocks = nvme_write_blocks;
	bdev.driver_data = ns;
	r = block_register(&bdev);
	if (r != VFS_OK) {
		kfree(ns);
		return r;
	}

	char msg[128];
	npf_snprintf(msg, sizeof(msg), "%s ready: nsid=%u blocks=%llu lba=%u",
				 ns->name, ns->nsid, (unsigned long long)ns->block_count,
				 ns->lba_size);
	driver_log(driver, "info", msg);
	return VFS_OK;
}

static int nvme_rw(block_device_t *bdev, uint64_t lba, uint32_t count,
				   void *buf, int write)
{
	nvme_ns_t *ns = bdev->driver_data;
	if (!ns || !ns->ctrl || !buf || count == 0 ||
		count * ns->lba_size > PAGE_SIZE)
		return VFS_ERR_INVAL;
	if (lba + count > ns->block_count)
		return VFS_ERR_INVAL;
	nvme_t *n = ns->ctrl;
	spinlock_acquire(&n->io_lock);
	if (write)
		memcpy(n->dma, buf, count * ns->lba_size);

	nvme_cmd_t cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.cdw0 = write ? NVME_IO_WRITE : NVME_IO_READ;
	cmd.nsid = ns->nsid;
	cmd.prp1 = n->dma_phys;
	cmd.cdw10 = (uint32_t)lba;
	cmd.cdw11 = (uint32_t)(lba >> 32);
	cmd.cdw12 = count - 1;
	int r = submit_wait(n, NVME_IO_QID, &cmd, NULL);
	if (r == VFS_OK && !write)
		memcpy(buf, n->dma, count * ns->lba_size);
	spinlock_release(&n->io_lock);
	return r;
}

static int nvme_read_blocks(block_device_t *bdev, uint64_t lba, uint32_t count,
							void *buf)
{
	nvme_ns_t *ns = bdev->driver_data;
	if (!ns || ns->lba_size == 0)
		return VFS_ERR_INVAL;
	uint8_t *dst = buf;
	uint32_t max_blocks = PAGE_SIZE / ns->lba_size;
	while (count) {
		uint32_t chunk = count < max_blocks ? count : max_blocks;
		int r = nvme_rw(bdev, lba, chunk, dst, 0);
		if (r != VFS_OK)
			return r;
		lba += chunk;
		dst += chunk * ns->lba_size;
		count -= chunk;
	}
	return VFS_OK;
}

static int nvme_write_blocks(block_device_t *bdev, uint64_t lba, uint32_t count,
							 const void *buf)
{
	nvme_ns_t *ns = bdev->driver_data;
	if (!ns || ns->lba_size == 0)
		return VFS_ERR_INVAL;
	const uint8_t *src = buf;
	uint32_t max_blocks = PAGE_SIZE / ns->lba_size;
	while (count) {
		uint32_t chunk = count < max_blocks ? count : max_blocks;
		int r = nvme_rw(bdev, lba, chunk, (void *)src, 1);
		if (r != VFS_OK)
			return r;
		lba += chunk;
		src += chunk * ns->lba_size;
		count -= chunk;
	}
	return VFS_OK;
}

static int nvme_match(const device_t *dev, void *ctx)
{
	(void)ctx;
	return dev && dev->bus_type == DEVICE_BUS_PCI && dev->pci.class_code == 0x01 &&
		   dev->pci.subclass == 0x08 && dev->pci.prog_if == 0x02;
}

static int nvme_probe(device_t *dev, void *ctx)
{
	driver_t *driver = ctx;
	const device_pci_info_t *p = &dev->pci;
	uint32_t bar0 = pci_read32(p, 0x10);
	uint32_t bar1 = pci_read32(p, 0x14);
	if ((bar0 & 1) || ((bar0 & 6) != 4))
		return VFS_ERR_INVAL;
	uint64_t bar = ((uint64_t)bar1 << 32) | (bar0 & ~0xFULL);

	nvme_t *n = kzalloc(sizeof(*n));
	if (!n)
		return VFS_ERR_NOMEM;
	n->driver = driver;
	n->queue_depth = NVME_QUEUE_DEPTH;
	n->next_cid = 1;
	spinlock_init(&n->io_lock);
	n->admin_cq_phase = 1;
	uint32_t id = next_ctrl_id++;

	n->mmio_virt = next_mmio;
	next_mmio += NVME_MMIO_STRIDE;
	map_mmio(kernel_ptable, n->mmio_virt, bar, 8);
	n->regs = (volatile uint8_t *)n->mmio_virt;
	uint64_t cap = nrd64(n, NVME_REG_CAP);
	n->db_stride = 4u << ((cap >> 32) & 0xF);

	uint16_t command = pci_read16(p, 0x04);
	pci_write16(p, 0x04, command | (1u << 1) | (1u << 2));

	uint32_t cc = nrd32(n, NVME_REG_CC);
	if (cc & NVME_CC_EN) {
		nwr32(n, NVME_REG_CC, cc & ~NVME_CC_EN);
		int r = wait_ready(n, 0);
		if (r != VFS_OK)
			return r;
	}

	n->asq = dma_page(&n->asq_phys);
	n->acq = dma_page(&n->acq_phys);
	n->dma = dma_page(&n->dma_phys);
	nwr32(n, NVME_REG_AQA,
		  ((n->queue_depth - 1) << 16) | (n->queue_depth - 1));
	nwr64(n, NVME_REG_ASQ, n->asq_phys);
	nwr64(n, NVME_REG_ACQ, n->acq_phys);
	nwr32(n, NVME_REG_CC,
		  NVME_CC_EN | NVME_CC_IOSQES_64 | NVME_CC_IOCQES_16);

	int r = wait_ready(n, 1);
	if (r == VFS_OK)
		r = create_io_queues(n);
	if (r != VFS_OK) {
		driver_log(driver, "err", "controller initialization failed");
		return r;
	}

	uint32_t nn = identify_controller_namespace_count(n);
	if (nn == 0) {
		driver_log(driver, "err", "controller exposes no namespaces");
		return VFS_ERR_NOENT;
	}

	unsigned registered = 0;
	for (uint32_t nsid = 1; nsid <= nn; nsid++) {
		r = register_namespace(driver, n, id, nsid);
		if (r == VFS_OK)
			registered++;
	}
	if (registered == 0)
		return VFS_ERR_NOENT;

	dev->driver_data = n;
	return VFS_OK;
}

static int nvme_main(driver_t *driver)
{
	device_handler_t handler;
	memset(&handler, 0, sizeof(handler));
	memcpy(handler.name, "nvme", 5);
	handler.bus_type = DEVICE_BUS_PCI;
	handler.match = nvme_match;
	handler.probe = nvme_probe;
	handler.ctx = driver;
	return device_handler_register(&handler);
}

static const char *const nvme_imports[] = {
	"_lyr_hhdm_offset", "block_register", "device_handler_register",
	"driver_log",	  "kernel_ptable", "kfree",
	"kzalloc",		  "map_mmio",	  "memcpy",
	"memset",		  "npf_snprintf_", "palloc_single",
	"strlen",
};

const driver_metadata_t lyr_driver_metadata = {
	.magic = DRIVER_MAGIC,
	.abi_version = DRIVER_ABI_VERSION,
	.name = "nvme",
	.entry = nvme_main,
	.imports = nvme_imports,
	.import_count = sizeof(nvme_imports) / sizeof(nvme_imports[0]),
};
