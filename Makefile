.DEFAULT_GOAL := all

ROOT_DIR   := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
BOOT_DIR   := $(ROOT_DIR)/axboot
KERNEL_DIR := $(ROOT_DIR)/kernel
export BUILD_DIR  := $(ROOT_DIR)/build

include mk/sources.mk
include mk/boot.mk

.PHONY: all clean kernel boot run sources

all: kernel

kernel: sources
	@$(MAKE) -C $(KERNEL_DIR)

boot: sources
	@$(MAKE) -C $(BOOT_DIR) ARCH=x86_64 PLATFORM=uefi

clean:
	$(MAKE) -C $(KERNEL_DIR) clean || true
	$(MAKE) -C $(BOOT_DIR) clean || true
	rm -f $(AXBOOT_HDR)
	rm lyr.iso