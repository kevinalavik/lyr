.SUFFIXES:

APPS := $(patsubst %/main.c,%,$(wildcard */main.c))
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
    -mcmodel=small \
    -Wno-unused-parameter

override CPPFLAGS := \
    -I include \
    -I ../kernel/include/sys \
    -isystem ../kernel/freestnd-c-hdrs/include \
    $(CPPFLAGS) \
    -MMD \
    -MP

override LDFLAGS += \
    -m elf_x86_64 \
    -nostdlib \
    -static \
    -z max-page-size=0x1000 \
    --gc-sections \
    -T linker.ld

BIN := $(addprefix $(OUTPUT_DIR)/,$(APPS))
CRT0_OBJ := $(OBJ_DIR)/crt0.S.o
APP_OBJ := $(addprefix $(OBJ_DIR)/,$(addsuffix /main.c.o,$(APPS)))
DEPS := $(APP_OBJ:.o=.d) $(CRT0_OBJ:.o=.d)

.PHONY: all
all: $(BIN)

-include $(DEPS)

.SECONDARY: $(CRT0_OBJ) $(APP_OBJ)

$(OUTPUT_DIR)/%: $(CRT0_OBJ) $(OBJ_DIR)/%/main.c.o GNUmakefile linker.ld
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(OBJ_DIR)/$*/main.c.o -o $@

$(OBJ_DIR)/%.c.o: %.c GNUmakefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(OBJ_DIR)/%.S.o: %.S GNUmakefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -rf $(OUTPUT_DIR) $(OBJ_DIR)

.PHONY: distclean
distclean: clean
