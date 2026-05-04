.SUFFIXES:

DRIVERS := pci e1000
OUTPUT_DIR := bin
OBJ_DIR := obj

TOOLCHAIN :=
TOOLCHAIN_PREFIX :=
ifneq ($(TOOLCHAIN),)
    ifeq ($(TOOLCHAIN_PREFIX),)
        TOOLCHAIN_PREFIX := $(TOOLCHAIN)-
    endif
endif

ifneq ($(TOOLCHAIN_PREFIX),)
    CC := $(TOOLCHAIN_PREFIX)gcc
else
    CC := cc
endif

LD := $(TOOLCHAIN_PREFIX)ld

ifeq ($(TOOLCHAIN),llvm)
    CC := clang
    LD := ld.lld
endif

override CC_IS_CLANG := $(shell ! $(CC) --version 2>/dev/null | grep -q '^Target: '; echo $$?)

ifeq ($(CC_IS_CLANG),1)
    override CC += -target x86_64-unknown-none-elf
endif

CFLAGS := -g -O2 -pipe
CPPFLAGS :=
LDFLAGS :=

override CFLAGS += \
    -Wall \
    -Wextra \
    -Werror \
    -std=gnu11 \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
    -fno-PIC \
    -ffunction-sections \
    -fdata-sections \
    -m64 \
    -march=x86-64 \
    -mabi=sysv \
    -mno-80387 \
    -mno-mmx \
    -mno-sse \
    -mno-sse2 \
    -mno-red-zone \
    -mcmodel=kernel \
    -Wno-unused-parameter

override CPPFLAGS := \
    -I ../kernel/include \
    -isystem ../kernel/freestnd-c-hdrs/include \
    $(CPPFLAGS) \
    -MMD \
    -MP

SYS := $(addprefix $(OUTPUT_DIR)/,$(addsuffix .sys,$(DRIVERS)))
OBJ := $(addprefix $(OBJ_DIR)/,$(addsuffix /main.c.o,$(DRIVERS)))
DEPS := $(OBJ:.o=.d)

.PHONY: all
all: $(SYS)

-include $(DEPS)

.SECONDARY: $(OBJ)

$(OUTPUT_DIR)/%.sys: $(OBJ_DIR)/%/main.c.o GNUmakefile
	mkdir -p "$(dir $@)"
	$(LD) -r $< -o $@

$(OBJ_DIR)/%/main.c.o: %/main.c GNUmakefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -rf $(OUTPUT_DIR) $(OBJ_DIR)

.PHONY: distclean
distclean: clean
