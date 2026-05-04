.SUFFIXES:

TAP_IF ?= tap0
QEMU_NET_USER := -device e1000,netdev=net0 -netdev user,id=net0,hostfwd=tcp::8080-:80
NVME_TEST_DISK := disk.img
QEMU_NVME := -drive file=$(NVME_TEST_DISK),if=none,id=nvme0,format=raw -device nvme,drive=nvme0,serial=LYRNVME0
QEMUFLAGS := -m 2G -smp 4 -serial stdio  $(QEMU_NET_USER) $(QEMU_NVME)

override IMAGE_NAME := lyr
INITRD_ROOT := initrd
ROOTFS_DIR := rootfs
INITRD_IMAGE := initrd.cpio
DRIVERS_ROOT := drivers
APPS_ROOT := src
APPS_BIN := $(APPS_ROOT)/bin
EARLY_INIT := early-init
EARLY_INIT_BIN := $(APPS_BIN)/$(EARLY_INIT)
INITRD_FILES := $(filter-out $(INITRD_IMAGE),$(shell find $(INITRD_ROOT) -type f -o -type d 2>/dev/null | LC_ALL=C sort))
DRIVER_SYS_FILES := $(shell find $(DRIVERS_ROOT)/bin -type f -name '*.sys' 2>/dev/null | LC_ALL=C sort)

HOST_CC := cc
HOST_CFLAGS := -g -O2 -pipe
HOST_CPPFLAGS :=
HOST_LDFLAGS :=
HOST_LIBS :=

.PHONY: all
all: $(IMAGE_NAME).iso rootfs-update disk-apps

.PHONY: all-hdd
all-hdd: $(IMAGE_NAME).hdd

.PHONY: run
run: $(IMAGE_NAME).iso rootfs-update disk-apps
	qemu-system-x86_64 \
		-M q35 \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS)

.PHONY: enable-usernet-icmp
enable-usernet-icmp:
	gid=$$(id -g); \
	printf "%s %s\n" "$$gid" "$$gid" | sudo tee /proc/sys/net/ipv4/ping_group_range

.PHONY: run-uefi
run-uefi: edk2-ovmf $(IMAGE_NAME).iso rootfs-update disk-apps
	qemu-system-x86_64 \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-x86_64.fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS)

.PHONY: run-hdd
run-hdd: $(IMAGE_NAME).hdd rootfs-update disk-apps
	qemu-system-x86_64 \
		-M q35 \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

.PHONY: run-hdd-uefi
run-hdd-uefi: edk2-ovmf $(IMAGE_NAME).hdd rootfs-update disk-apps
	qemu-system-x86_64 \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-x86_64.fd,readonly=on \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

edk2-ovmf:
	curl -L https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/edk2-ovmf.tar.gz | gunzip | tar -xf -

limine/limine:
	rm -rf limine
	git clone https://github.com/Limine-Bootloader/Limine.git limine --branch=v11.x-binary --depth=1
	$(MAKE) -C limine \
		CC="$(HOST_CC)" \
		CFLAGS="$(HOST_CFLAGS)" \
		CPPFLAGS="$(HOST_CPPFLAGS)" \
		LDFLAGS="$(HOST_LDFLAGS)" \
		LIBS="$(HOST_LIBS)"

kernel/.deps-obtained:
	./kernel/get-deps

.PHONY: kernel
kernel: kernel/.deps-obtained
	$(MAKE) -C kernel

.PHONY: drivers
drivers: kernel/.deps-obtained
	$(MAKE) -C drivers

.PHONY: apps
apps: kernel/.deps-obtained
	$(MAKE) -C $(APPS_ROOT)

.PHONY: disk-apps
disk-apps: apps
	@if [ ! -f "$(NVME_TEST_DISK)" ]; then \
		$(MAKE) $(NVME_TEST_DISK); \
	fi
	PATH=$$PATH:/usr/sbin:/sbin; \
	debugfs -w -R "mkdir /bin" $(NVME_TEST_DISK) >/dev/null 2>&1 || true; \
	for app in $(APPS_BIN)/*; do \
		[ -f "$$app" ] || continue; \
		[ "$$(basename "$$app")" != "$(EARLY_INIT)" ] || continue; \
		dst="/bin/$$(basename "$$app")"; \
		if debugfs -R "stat $$dst" $(NVME_TEST_DISK) 2>&1 | grep -q '^Inode:'; then \
			echo "exists: $$dst"; \
		else \
			echo "write: $$dst"; \
			debugfs -w -R "write $$app $$dst" $(NVME_TEST_DISK); \
		fi; \
	done

.PHONY: rootfs-update
rootfs-update:
	@if [ ! -f "$(NVME_TEST_DISK)" ]; then \
		$(MAKE) $(NVME_TEST_DISK); \
	fi
	PATH=$$PATH:/usr/sbin:/sbin; \
	find $(ROOTFS_DIR) -type f | while read src; do \
		rel=$${src#$(ROOTFS_DIR)/}; \
		dir=$$(dirname "$$rel"); \
		if [ "$$dir" != "." ]; then \
			debugfs -w -R "mkdir /$$dir" $(NVME_TEST_DISK) >/dev/null 2>&1 || true; \
		fi; \
		if debugfs -R "stat /$$rel" $(NVME_TEST_DISK) 2>&1 | grep -q '^Inode:'; then \
			echo "exists: /$$rel"; \
		else \
			echo "write: /$$rel"; \
			debugfs -w -R "write $$src /$$rel" $(NVME_TEST_DISK); \
		fi; \
	done
	
.PHONY: rootfs-extract
rootfs-extract: $(NVME_TEST_DISK)
	rm -rf $(ROOTFS_DIR)
	mkdir -p $(ROOTFS_DIR)
	PATH=$$PATH:/usr/sbin:/sbin; \
	debugfs -R "ls -l /" $(NVME_TEST_DISK) 2>/dev/null | awk 'NF>1{print $$NF}' | grep -v '^\.' | while read entry; do \
		debugfs -R "rdump /$$entry $(ROOTFS_DIR)/$$entry" $(NVME_TEST_DISK) 2>/dev/null || true; \
	done

.PHONY: FORCE
FORCE:

$(NVME_TEST_DISK):
	dd if=/dev/zero of=$@ bs=1M count=16
	PATH=$$PATH:/usr/sbin:/sbin mkfs.ext2 -q -F -L LYRTEST $@

$(INITRD_IMAGE): FORCE utils/mkinitrd.py $(INITRD_FILES) drivers apps $(DRIVER_SYS_FILES)
	python3 utils/mkinitrd.py $(INITRD_ROOT) $@ $(DRIVERS_ROOT)/bin:sys $(EARLY_INIT_BIN):/$(EARLY_INIT)

$(IMAGE_NAME).iso: limine/limine kernel $(INITRD_IMAGE)
	rm -rf iso_root
	mkdir -p iso_root/boot
	cp -v kernel/bin/lyr iso_root/boot/lyr.efi
	cp -v $(INITRD_IMAGE) iso_root/boot/initrd.cpio
	mkdir -p iso_root/boot/limine
	cp -v limine.conf limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/boot/limine/
	mkdir -p iso_root/EFI/BOOT
	cp -v limine/BOOTX64.EFI iso_root/EFI/BOOT/
	cp -v limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(IMAGE_NAME).iso
	./limine/limine bios-install $(IMAGE_NAME).iso
	rm -rf iso_root

$(IMAGE_NAME).hdd: limine/limine kernel
	rm -f $(IMAGE_NAME).hdd
	dd if=/dev/zero bs=1M count=0 seek=64 of=$(IMAGE_NAME).hdd
	PATH=$$PATH:/usr/sbin:/sbin sgdisk $(IMAGE_NAME).hdd -n 1:2048 -t 1:ef00 -m 1
	./limine/limine bios-install $(IMAGE_NAME).hdd
	mformat -i $(IMAGE_NAME).hdd@@1M
	mmd -i $(IMAGE_NAME).hdd@@1M ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	mcopy -i $(IMAGE_NAME).hdd@@1M kernel/bin/kernel ::/boot
	mcopy -i $(IMAGE_NAME).hdd@@1M limine.conf limine/limine-bios.sys ::/boot/limine
	mcopy -i $(IMAGE_NAME).hdd@@1M limine/BOOTX64.EFI ::/EFI/BOOT
	mcopy -i $(IMAGE_NAME).hdd@@1M limine/BOOTIA32.EFI ::/EFI/BOOT

.PHONY: clean
clean:
	$(MAKE) -C kernel clean
	$(MAKE) -C drivers clean
	$(MAKE) -C $(APPS_ROOT) clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).hdd $(INITRD_IMAGE)

.PHONY: distclean
distclean: clean
	$(MAKE) -C kernel distclean
	$(MAKE) -C drivers distclean
	$(MAKE) -C $(APPS_ROOT) distclean
	rm -rf limine edk2-ovmf $(NVME_TEST_DISK)