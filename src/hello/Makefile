.SUFFIXES:

PREFIX ?= /usr
DESTDIR ?=

PROJECT := hello
PROJECT_SRC := $(wildcard src/*.c)

TOOLCHAIN_PREFIX ?= x86_64-lyr-
CC := $(TOOLCHAIN_PREFIX)gcc

OUTPUT_DIR := bin
OBJ_DIR := obj

PROJECT_OBJ := $(patsubst %.c,$(OBJ_DIR)/%.c.o,$(PROJECT_SRC))

CFLAGS := -g -O2 -pipe -Wall -Wextra -Werror -std=gnu11
CFLAGS += -I include 

LDFLAGS :=

.PHONY: all install clean distclean

all: $(OUTPUT_DIR)/$(PROJECT)

$(OUTPUT_DIR)/$(PROJECT): $(PROJECT_OBJ)
	mkdir -p "$(dir $@)"
	$(CC) $(LDFLAGS) $^ -o $@

$(OBJ_DIR)/%.c.o: %.c Makefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) -c $< -o $@

install: all
	install -D -m 755 $(OUTPUT_DIR)/$(PROJECT) $(DESTDIR)$(PREFIX)/bin/$(PROJECT)

clean:
	rm -rf $(OBJ_DIR) $(OUTPUT_DIR)

distclean: clean
