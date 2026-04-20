BUILD_DIR   := $(ROOT_DIR)/build

BOOT_FILE   := $(BUILD_DIR)/boot/uefi/BOOTX64.EFI
KERNEL_FILE := $(BUILD_DIR)/lyr.elf

ISO_FILE    := $(ROOT_DIR)/lyr.iso
UEFI_IMG    := $(BUILD_DIR)/uefi.img
NVRAM_FILE  := $(BUILD_DIR)/nvram.json

OVMF_CODE   := $(ROOT_DIR)/ovmf/ovmf_code-x86_64.fd
OVMF_VARS   := $(ROOT_DIR)/ovmf/ovmf_vars-x86_64.fd

QEMU_FLAGS  := -m 2G -smp 4 -rtc base=localtime -M q35 -serial stdio

all: $(ISO_FILE)

run: $(ISO_FILE)
	@qemu-system-x86_64 $(QEMU_FLAGS) \
		-drive if=pflash,format=raw,unit=0,file=$(OVMF_CODE),readonly=on \
		-drive if=pflash,format=raw,unit=1,file=$(OVMF_VARS) \
		-device uefi-vars-x64,jsonfile=$(NVRAM_FILE) \
		-cdrom $(ISO_FILE) -d guest_errors


$(ISO_FILE): $(UEFI_IMG)
	@rm -rf iso_root
	@mkdir -p iso_root/boot
	@mkdir -p iso_root/EFI/BOOT

	@cp $(UEFI_IMG) iso_root/boot/uefi.img
	@cp $(BOOT_FILE) iso_root/EFI/BOOT/BOOTX64.EFI

	@xorriso -as mkisofs -R -r -J \
		-eltorito-alt-boot \
		-e boot/uefi.img \
		-no-emul-boot \
		-eltorito-platform efi \
		-efi-boot-part --efi-boot-image \
		--protective-msdos-label \
		iso_root -o $(ISO_FILE)

	@rm -rf iso_root

$(UEFI_IMG): $(BOOT_FILE) $(KERNEL_FILE)
	@echo "[*] Creating UEFI FAT image"

	@dd if=/dev/zero of=$(UEFI_IMG) bs=1M count=64 status=none
	@mformat -i $(UEFI_IMG) ::

	@mmd -i $(UEFI_IMG) ::/EFI
	@mmd -i $(UEFI_IMG) ::/EFI/BOOT
	@mmd -i $(UEFI_IMG) ::/System

	@mcopy -i $(UEFI_IMG) $(BOOT_FILE) ::/EFI/BOOT/BOOTX64.EFI
	@mcopy -i $(UEFI_IMG) $(KERNEL_FILE) ::/System/axkrnl

	@echo "[OK] UEFI image ready"
