#ifndef _LYR_LIB_ELF_H
#define _LYR_LIB_ELF_H

#include <stddef.h>
#include <stdint.h>

#define ELF_EI_NIDENT 16
#define ELF_SHN_UNDEF 0

typedef uint16_t elf64_half_t;
typedef uint32_t elf64_word_t;
typedef int32_t elf64_sword_t;
typedef uint64_t elf64_xword_t;
typedef int64_t elf64_sxword_t;
typedef uint64_t elf64_addr_t;
typedef uint64_t elf64_off_t;

typedef struct {
	unsigned char e_ident[ELF_EI_NIDENT];
	elf64_half_t e_type;
	elf64_half_t e_machine;
	elf64_word_t e_version;
	elf64_addr_t e_entry;
	elf64_off_t e_phoff;
	elf64_off_t e_shoff;
	elf64_word_t e_flags;
	elf64_half_t e_ehsize;
	elf64_half_t e_phentsize;
	elf64_half_t e_phnum;
	elf64_half_t e_shentsize;
	elf64_half_t e_shnum;
	elf64_half_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
	elf64_word_t sh_name;
	elf64_word_t sh_type;
	elf64_xword_t sh_flags;
	elf64_addr_t sh_addr;
	elf64_off_t sh_offset;
	elf64_xword_t sh_size;
	elf64_word_t sh_link;
	elf64_word_t sh_info;
	elf64_xword_t sh_addralign;
	elf64_xword_t sh_entsize;
} elf64_shdr_t;

typedef struct {
	elf64_word_t st_name;
	unsigned char st_info;
	unsigned char st_other;
	elf64_half_t st_shndx;
	elf64_addr_t st_value;
	elf64_xword_t st_size;
} elf64_sym_t;

typedef struct {
	elf64_addr_t r_offset;
	elf64_xword_t r_info;
	elf64_sxword_t r_addend;
} elf64_rela_t;

typedef struct {
	uint8_t *file;
	size_t file_size;
	void **sections;
	const elf64_shdr_t *sections_hdr;
	size_t section_count;
	const elf64_sym_t *symtab;
	size_t sym_count;
	const char *strtab;
} elf_image_t;

typedef void *(*elf_alloc_section_t)(uint64_t size, uint64_t align, void *ctx);
typedef int (*elf_resolve_symbol_t)(const char *name, uint64_t *out, void *ctx);

int elf_load_relocatable(elf_image_t *image, uint8_t *file, size_t file_size,
						 elf_alloc_section_t alloc, void *alloc_ctx,
						 elf_resolve_symbol_t resolve, void *resolve_ctx);
const char *elf_symbol_name(const elf_image_t *image, const elf64_sym_t *sym);
int elf_symbol_value(const elf_image_t *image, size_t index, uint64_t *out,
					 elf_resolve_symbol_t resolve, void *resolve_ctx);
int elf_find_symbol_value(const elf_image_t *image, const char *name,
						  uint64_t *out, elf_resolve_symbol_t resolve,
						  void *resolve_ctx);
int elf_find_defined_symbol_value(const elf_image_t *image, const char *name,
								  uint64_t *out);

#endif /* _LYR_LIB_ELF_H */
