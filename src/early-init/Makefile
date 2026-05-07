.SUFFIXES:

PREFIX ?= /usr
DESTDIR ?=

PROJECT := early-init
PROJECT_SRC := main.c

CC ?= cc
LD ?= ld

OUTPUT_DIR := bin
OBJ_DIR := obj

CFLAGS := -g -O2 -pipe -Wall -Wextra -Werror -std=gnu11 -ffreestanding -Wno-unused-parameter
CFLAGS += -fno-stack-protector -fno-stack-check -fno-lto -fno-PIC
CFLAGS += -ffunction-sections -fdata-sections -m64 -march=x86-64 -mabi=sysv
CFLAGS += -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -mcmodel=small
CFLAGS += -I ../include -I ../../kernel/include/sys -isystem ../../kernel/freestnd-c-hdrs/include
LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 --gc-sections -T linker.ld

.PHONY: all install clean distclean

all: $(OUTPUT_DIR)/$(PROJECT)

clean:
	rm -rf $(OUTPUT_DIR) $(OBJ_DIR)

distclean: clean

$(OUTPUT_DIR)/$(PROJECT): $(OBJ_DIR)/crt0.S.o $(OBJ_DIR)/$(PROJECT_SRC).o
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) $< $(OBJ_DIR)/$(PROJECT_SRC).o -o $@

$(OBJ_DIR)/%.c.o: %.c Makefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.S.o: %.S Makefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/crt0.S.o: ../crt0.S Makefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

# special app so install to (prefix)/early-init
install: all
	install -D -m 755 $(OUTPUT_DIR)/$(PROJECT) $(DESTDIR)$(PREFIX)/$(PROJECT)