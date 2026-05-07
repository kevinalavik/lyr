.SUFFIXES:

PREFIX ?= /usr
DESTDIR ?=

PROJECT := init
PROJECT_SRC := main.c

TOOLCHAIN_PREFIX ?= x86_64-lyr-
CC := $(TOOLCHAIN_PREFIX)gcc
LD := $(TOOLCHAIN_PREFIX)ld

OUTPUT_DIR := bin
OBJ_DIR := obj

CFLAGS := -g -O2 -pipe -Wall -Wextra -Werror -std=gnu11
CFLAGS += -I include -I ../../kernel/include/sys -isystem $(shell $(TOOLCHAIN_PREFIX)gcc -print-sysroot)/usr/include
LDFLAGS := -static -nostdlib

.PHONY: all install clean distclean

all: $(OUTPUT_DIR)/$(PROJECT)

$(OUTPUT_DIR)/$(PROJECT): $(OBJ_DIR)/$(PROJECT_SRC).o
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) $< -o $@

$(OBJ_DIR)/%.c.o: %.c Makefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

install: all
	install -D -m 755 $(OUTPUT_DIR)/$(PROJECT) $(DESTDIR)$(PREFIX)/bin/$(PROJECT)