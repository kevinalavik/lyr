CC ?= gcc
LD ?= ld

ifndef BUILD_DIR
BUILD_DIR := build
endif

DESTDIR ?=
BOOTDIR ?= /boot

TARGET := $(BUILD_DIR)/lyr.elf
INSTALL_TARGET := $(DESTDIR)$(BOOTDIR)/lyr.elf

CPPFLAGS := -Iinclude -D__KERNEL__ -D__X86_64__

ARCH_CFLAGS := \
	-m64 \
	-march=x86-64 \
	-mabi=sysv \
	-mcmodel=kernel \
	-mno-red-zone \
	-mno-80387 \
	-mno-mmx \
	-mno-sse \
	-mno-sse2

CFLAGS := \
	-std=c99 \
	-Wall \
	-Wextra \
	-Werror \
	-ffreestanding \
	-fno-omit-frame-pointer \
	-fno-stack-protector \
	-fno-stack-check \
	-fno-pic \
	-fno-pie \
	-MMD \
	-MP \
	$(ARCH_CFLAGS)

LDFLAGS := \
	-T linker.ld \
	-nostdlib \
	-static \
	--no-dynamic-linker \
	-z max-page-size=0x1000 \
	-z text \
	-m elf_x86_64

SRC := $(shell find . -type f -name '*.c' -not -path './$(BUILD_DIR)/*')
OBJ := $(patsubst ./%.c,$(BUILD_DIR)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

install: $(TARGET)
	install -d $(dir $(INSTALL_TARGET))
	install -m 644 $(TARGET) $(INSTALL_TARGET)

uninstall:
	rm -f $(INSTALL_TARGET)

reinstall: uninstall install

clean:
	rm -rf $(BUILD_DIR)

-include $(DEP)

.PHONY: all install uninstall reinstall clean