.SUFFIXES:

TAP_IF ?= tap0
QEMU_NET_USER := -netdev user,id=net0,net=10.0.2.0/24,hostfwd=tcp::6969-:6969,hostfwd=tcp::8080-:80 -device e1000,netdev=net0
ROOTFS_DISK := disk.img
QEMU_NVME := -drive file=$(ROOTFS_DISK),if=none,id=nvme0,format=raw -device nvme,drive=nvme0,serial=LYRNVME0
QEMUFLAGS := -m 8G -smp 4 -serial stdio  $(QEMU_NET_USER) $(QEMU_NVME)

override IMAGE_NAME := lyr
INITRD_ROOT := initrd
ROOTFS_DIR := rootfs
SYSTEM_ROOT := system-root
INITRD_IMAGE := initrd.cpio
DRIVERS_ROOT := drivers
EARLY_INIT := early-init
EARLY_INIT_DIR := src/early-init
INITRD_FILES := $(filter-out $(INITRD_IMAGE),$(shell find $(INITRD_ROOT) -type f -o -type d 2>/dev/null | LC_ALL=C sort))
DRIVER_SYS_FILES := $(shell find $(DRIVERS_ROOT)/bin -type f -name '*.sys' 2>/dev/null | LC_ALL=C sort)

HOST_CC := cc
HOST_CFLAGS := -g -O2 -pipe
HOST_CPPFLAGS :=
HOST_LDFLAGS :=
HOST_LIBS :=

.PHONY: all
all: $(IMAGE_NAME).iso rootfs-update

.PHONY: all-hdd
all-hdd: $(IMAGE_NAME).hdd

.PHONY: run
run: $(IMAGE_NAME).iso rootfs-update
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
run-uefi: edk2-ovmf $(IMAGE_NAME).iso rootfs-update
	qemu-system-x86_64 \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-x86_64.fd,readonly=on \
		-cdrom $(IMAGE_NAME).iso \
		-boot d \
		$(QEMUFLAGS)

.PHONY: run-hdd
run-hdd: $(IMAGE_NAME).hdd rootfs-update
	qemu-system-x86_64 \
		-M q35 \
		-hda $(IMAGE_NAME).hdd \
		$(QEMUFLAGS)

.PHONY: run-hdd-uefi
run-hdd-uefi: edk2-ovmf $(IMAGE_NAME).hdd rootfs-update
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

.PHONY: early-init
early-init: kernel/.deps-obtained
	$(MAKE) -C $(EARLY_INIT_DIR) install \
		DESTDIR="$(abspath $(INITRD_ROOT))" \
		PREFIX=/

.PHONY: rootfs-update
rootfs-update:
	@if [ ! -f "$(ROOTFS_DISK)" ]; then \
		$(MAKE) $(ROOTFS_DISK); \
	fi
	@PATH=$$PATH:/usr/sbin:/sbin; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	{ \
		if [ -d "$(ROOTFS_DIR)" ]; then \
			find "$(ROOTFS_DIR)" -type d -printf '%P\n'; \
		fi; \
		if [ -d "$(SYSTEM_ROOT)" ]; then \
			find "$(SYSTEM_ROOT)" -type d -printf '%P\n'; \
		fi; \
	} | awk 'NF' | LC_ALL=C sort -u > "$$tmp/dirs"; \
	while IFS= read -r dir; do \
		debugfs -w -R "mkdir /$$dir" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
		case "$$dir" in \
			home/lyr/*) \
				debugfs -w -R "set_inode_field /$$dir uid 1000" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
				debugfs -w -R "set_inode_field /$$dir gid 1000" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
				debugfs -w -R "set_inode_field /$$dir mode 040755" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
			;; \
		esac; \
	done < "$$tmp/dirs"; \
	{ \
		if [ -d "$(ROOTFS_DIR)" ]; then \
			find "$(ROOTFS_DIR)" -type f -printf '%P\n'; \
		fi; \
		if [ -d "$(SYSTEM_ROOT)" ]; then \
			find "$(SYSTEM_ROOT)" -type f -printf '%P\n'; \
		fi; \
	} | awk 'NF' | LC_ALL=C sort -u > "$$tmp/files"; \
	while IFS= read -r rel; do \
		src="$(ROOTFS_DIR)/$$rel"; \
		if [ -f "$(SYSTEM_ROOT)/$$rel" ]; then \
			src="$(SYSTEM_ROOT)/$$rel"; \
		fi; \
		debugfs -w -R "rm /$$rel" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
		debugfs -w -R "write $$src /$$rel" "$(ROOTFS_DISK)" >/dev/null 2>&1; \
		case "$$rel" in \
			home/lyr/*) \
				debugfs -w -R "set_inode_field /$$rel uid 1000" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
				debugfs -w -R "set_inode_field /$$rel gid 1000" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
			;; \
		esac; \
	done < "$$tmp/files"
	@PATH=$$PATH:/usr/sbin:/sbin; \
	debugfs -w -R "mkdir /home" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
	debugfs -w -R "mkdir /home/lyr" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
	debugfs -w -R "set_inode_field / uid 0" "$(ROOTFS_DISK)" >/dev/null; \
	debugfs -w -R "set_inode_field / gid 0" "$(ROOTFS_DISK)" >/dev/null; \
	debugfs -w -R "set_inode_field / mode 040755" "$(ROOTFS_DISK)" >/dev/null; \
	debugfs -w -R "set_inode_field /root uid 0" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
	debugfs -w -R "set_inode_field /root gid 0" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
	debugfs -w -R "set_inode_field /root mode 040700" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
	debugfs -w -R "set_inode_field /home uid 0" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
	debugfs -w -R "set_inode_field /home gid 0" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
	debugfs -w -R "set_inode_field /home mode 040755" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
	debugfs -w -R "set_inode_field /home/lyr uid 1000" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
	debugfs -w -R "set_inode_field /home/lyr gid 1000" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true; \
	debugfs -w -R "set_inode_field /home/lyr mode 040755" "$(ROOTFS_DISK)" >/dev/null 2>&1 || true

.PHONY: rootfs-extract
rootfs-extract: $(ROOTFS_DISK)
	rm -rf $(ROOTFS_DIR)
	mkdir -p $(ROOTFS_DIR)
	PATH=$$PATH:/usr/sbin:/sbin; \
	debugfs -R "ls -l /" $(ROOTFS_DISK) 2>/dev/null | awk 'NF>1{print $$NF}' | grep -v '^\.' | while read entry; do \
		debugfs -R "rdump /$$entry $(ROOTFS_DIR)/$$entry" $(ROOTFS_DISK) 2>/dev/null || true; \
	done

.PHONY: FORCE
FORCE:

$(ROOTFS_DISK):
	truncate -s 1G $@
	PATH=$$PATH:/usr/sbin:/sbin mkfs.ext2 -q -F -L LYRTEST $@

$(INITRD_IMAGE): FORCE utils/mkinitrd.py $(INITRD_FILES) drivers early-init $(DRIVER_SYS_FILES)
	python3 utils/mkinitrd.py $(INITRD_ROOT) $@ $(DRIVERS_ROOT)/bin:sys

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
	$(MAKE) -C $(EARLY_INIT_DIR) clean
	rm -rf iso_root $(IMAGE_NAME).iso $(IMAGE_NAME).hdd $(INITRD_IMAGE)

.PHONY: distclean
distclean: clean
	$(MAKE) -C kernel distclean
	$(MAKE) -C drivers distclean
	$(MAKE) -C $(EARLY_INIT_DIR) distclean
	rm -rf limine edk2-ovmf $(ROOTFS_DISK)