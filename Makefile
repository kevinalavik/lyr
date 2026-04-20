CC      ?= gcc
LD      ?= ld
BUILD   ?= build
DESTDIR ?=

ROOT    := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

KERNEL  := $(ROOT)/kernel
BOOT    := $(ROOT)/axboot
OVMF    := $(ROOT)/ovmf

TARGET      := $(abspath $(BUILD))/lyr.elf
UEFI_IMG    := $(abspath $(BUILD))/uefi.img
NVRAM_FILE  := $(abspath $(BUILD))/nvram.json
ISO_FILE    := $(ROOT)/lyr.iso

OVMF_CODE  := $(OVMF)/ovmf_code-x86_64.fd
OVMF_VARS  := $(OVMF)/ovmf_vars-x86_64.fd

QEMU_FLAGS := -m 2G -smp 4 -rtc base=localtime -M q35 -serial stdio

$(TARGET): $(wildcard $(KERNEL)/src/*.c)
	@$(MAKE) -C $(KERNEL) BUILD_DIR=$(abspath $(BUILD))

$(BOOT):
	@$(ROOT)/utils/fetch.sh -t git -o $@ https://github.com/piraterna/axboot

$(KERNEL)/include/boot/axboot.h:
	@mkdir -p $(dir $@)
	@$(ROOT)/utils/fetch.sh -t file -o $@ https://raw.githubusercontent.com/piraterna/axboot/main/include/proto/aurix.h

$(OVMF):
	@mkdir -p $(dir $@)
	@$(ROOT)/utils/fetch.sh -t git -o $@ https://github.com/piraterna/ovmf-bins

sources: $(BOOT) $(KERNEL)/include/boot/axboot.h $(OVMF)

$(ISO_FILE): $(UEFI_IMG) $(TARGET) $(BUILD)/boot/uefi/BOOTX64.EFI
	@echo "[*] Creating ISO image"
	@mkdir -p $(BUILD)/iso_root/boot $(BUILD)/iso_root/EFI/BOOT
	@cp $(UEFI_IMG) $(BUILD)/iso_root/boot/uefi.img
	@cp $(BUILD)/boot/uefi/BOOTX64.EFI $(BUILD)/iso_root/EFI/BOOT/
	@xorriso -as mkisofs -R -r -J -eltorito-alt-boot -e boot/uefi.img -no-emul-boot -eltorito-platform efi -efi-boot-part --efi-boot-image --protective-msdos-label $(BUILD)/iso_root -o $(ISO_FILE)
	@rm -rf $(BUILD)/iso_root
	@echo "[OK] ISO ready: $(ISO_FILE)"

$(UEFI_IMG): $(BUILD)/boot/uefi/BOOTX64.EFI $(TARGET) | sources
	@echo "[*] Creating UEFI FAT image"
	@mkdir -p $(abspath $(BUILD))
	@dd if=/dev/zero of=$(UEFI_IMG) bs=1M count=64 status=none
	@mformat -i $(UEFI_IMG) ::
	@mmd -i $(UEFI_IMG) ::/EFI ::/EFI/BOOT ::/System ::/AxBoot
	@mcopy -i $(UEFI_IMG) $(BUILD)/boot/uefi/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	@mcopy -i $(UEFI_IMG) $(TARGET) ::/System/axkrnl
	@mcopy -i $(UEFI_IMG) $(BOOT)/base/axboot.cfg ::/AxBoot/axboot.cfg -s
	@echo "[OK] UEFI image ready"

$(BUILD)/boot/uefi/BOOTX64.EFI: | $(BOOT)
	@mkdir -p $(dir $@)
	@$(MAKE) -C $(BOOT) ARCH=x86_64 PLATFORM=uefi BUILD_DIR=$(BUILD)/boot
	@cp $(BOOT)/platform/uefi/build/boot/boot/uefi/BOOTX64.EFI $@

.PHONY: all
all: $(ISO_FILE)

.PHONY: run
run: $(ISO_FILE)
	@qemu-system-x86_64 $(QEMU_FLAGS) \
		-drive if=pflash,format=raw,unit=0,file=$(OVMF_CODE),readonly=on \
		-drive if=pflash,format=raw,unit=1,file=$(OVMF_VARS) \
		-device uefi-vars-x64,jsonfile=$(NVRAM_FILE) \
		-cdrom $(ISO_FILE) -d guest_errors

.PHONY: clean
clean:
	@$(MAKE) -C $(KERNEL) clean 2>/dev/null || true
	@rm -rf $(BUILD)
	@rm -f $(ISO_FILE)

.PHONY: install
install: $(TARGET)
	install -d $(DESTDIR)/boot
	install -m 644 $(TARGET) $(DESTDIR)/boot/lyr.elf

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)/boot/lyr.elf