#include <lib/elf.h>
#include <debug/log.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/page.h>

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_REL 1
#define EM_X86_64 62
#define SHF_ALLOC 0x2
#define SHT_SYMTAB 2
#define SHT_RELA 4
#define SHT_NOBITS 8
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_PLT32 4
#define R_X86_64_32 10
#define R_X86_64_32S 11

#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((uint32_t)(i))

static int range_ok(size_t file_size, uint64_t off, uint64_t size)
{
	return off <= file_size && size <= file_size - off;
}

const char *elf_symbol_name(const elf_image_t *image, const elf64_sym_t *sym)
{
	if (!image || !sym || !image->strtab)
		return "";
	return image->strtab + sym->st_name;
}

static int elf_validate(const elf64_ehdr_t *ehdr, size_t file_size)
{
	if (file_size < sizeof(*ehdr))
		return VFS_ERR_INVAL;
	if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
		ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3 ||
		ehdr->e_ident[4] != ELFCLASS64 || ehdr->e_ident[5] != ELFDATA2LSB)
		return VFS_ERR_INVAL;
	if (ehdr->e_type != ET_REL || ehdr->e_machine != EM_X86_64)
		return VFS_ERR_INVAL;
	if (ehdr->e_shentsize != sizeof(elf64_shdr_t) || ehdr->e_shnum == 0 ||
		!range_ok(file_size, ehdr->e_shoff,
				  (uint64_t)ehdr->e_shentsize * ehdr->e_shnum))
		return VFS_ERR_INVAL;
	return VFS_OK;
}

static int elf_load_sections(elf_image_t *image, const elf64_ehdr_t *ehdr,
							 elf_alloc_section_t alloc, void *alloc_ctx)
{
	image->section_count = ehdr->e_shnum;
	image->sections_hdr = (const elf64_shdr_t *)(image->file + ehdr->e_shoff);
	image->sections = kzalloc(image->section_count * sizeof(void *));
	if (!image->sections)
		return VFS_ERR_NOMEM;

	for (size_t i = 0; i < image->section_count; i++) {
		const elf64_shdr_t *sh = &image->sections_hdr[i];
		if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0)
			continue;

		uint64_t align = sh->sh_addralign ? sh->sh_addralign : PAGE_SIZE;
		uint8_t *mem = alloc ? alloc(sh->sh_size, align, alloc_ctx) : NULL;
		if (!mem)
			return VFS_ERR_NOMEM;
		if (sh->sh_type == SHT_NOBITS) {
			memset(mem, 0, (size_t)sh->sh_size);
		} else {
			if (!range_ok(image->file_size, sh->sh_offset, sh->sh_size))
				return VFS_ERR_INVAL;
			memcpy(mem, image->file + sh->sh_offset, (size_t)sh->sh_size);
		}
		image->sections[i] = mem;
	}
	return VFS_OK;
}

static int elf_find_symbols(elf_image_t *image)
{
	for (size_t i = 0; i < image->section_count; i++) {
		const elf64_shdr_t *sh = &image->sections_hdr[i];
		if (sh->sh_type != SHT_SYMTAB)
			continue;
		if (sh->sh_entsize != sizeof(elf64_sym_t) ||
			!range_ok(image->file_size, sh->sh_offset, sh->sh_size) ||
			sh->sh_link >= image->section_count)
			return VFS_ERR_INVAL;

		const elf64_shdr_t *str = &image->sections_hdr[sh->sh_link];
		if (!range_ok(image->file_size, str->sh_offset, str->sh_size))
			return VFS_ERR_INVAL;
		image->symtab = (const elf64_sym_t *)(image->file + sh->sh_offset);
		image->sym_count = (size_t)(sh->sh_size / sh->sh_entsize);
		image->strtab = (const char *)(image->file + str->sh_offset);
		return VFS_OK;
	}
	return VFS_ERR_INVAL;
}

int elf_symbol_value(const elf_image_t *image, size_t index, uint64_t *out,
					 elf_resolve_symbol_t resolve, void *resolve_ctx)
{
	if (!image || !out || index >= image->sym_count)
		return VFS_ERR_INVAL;

	const elf64_sym_t *sym = &image->symtab[index];
	if (sym->st_shndx == ELF_SHN_UNDEF) {
		if (!resolve)
			return VFS_ERR_NOENT;
		return resolve(elf_symbol_name(image, sym), out, resolve_ctx);
	}
	if (sym->st_shndx >= image->section_count ||
		!image->sections[sym->st_shndx])
		return VFS_ERR_INVAL;
	*out = (uint64_t)image->sections[sym->st_shndx] + sym->st_value;
	return VFS_OK;
}

static int elf_apply_rela(elf_image_t *image, const elf64_rela_t *rela,
						  uint8_t *target, elf_resolve_symbol_t resolve,
						  void *resolve_ctx)
{
	uint64_t s = 0;
	int r = elf_symbol_value(image, (size_t)ELF64_R_SYM(rela->r_info), &s,
							 resolve, resolve_ctx);
	if (r != VFS_OK)
		return r;

	uint64_t p = (uint64_t)(target + rela->r_offset);
	uint64_t v = s + (uint64_t)rela->r_addend;
	switch (ELF64_R_TYPE(rela->r_info)) {
	case R_X86_64_64:
		*(uint64_t *)p = v;
		return VFS_OK;
	case R_X86_64_PC32:
	case R_X86_64_PLT32:
		*(uint32_t *)p = (uint32_t)(v - p);
		return VFS_OK;
	case R_X86_64_32:
	case R_X86_64_32S:
		*(uint32_t *)p = (uint32_t)v;
		return VFS_OK;
	default:
		log_err("elf", "unsupported relocation type=%u",
				ELF64_R_TYPE(rela->r_info));
		return VFS_ERR_NOSYS;
	}
}

static int elf_apply_relocations(elf_image_t *image,
								 elf_resolve_symbol_t resolve,
								 void *resolve_ctx)
{
	for (size_t i = 0; i < image->section_count; i++) {
		const elf64_shdr_t *sh = &image->sections_hdr[i];
		if (sh->sh_type != SHT_RELA)
			continue;
		if (sh->sh_info >= image->section_count)
			return VFS_ERR_INVAL;
		if (!(image->sections_hdr[sh->sh_info].sh_flags & SHF_ALLOC))
			continue;
		if (sh->sh_entsize != sizeof(elf64_rela_t) ||
			!image->sections[sh->sh_info] ||
			!range_ok(image->file_size, sh->sh_offset, sh->sh_size))
			return VFS_ERR_INVAL;

		const elf64_rela_t *relas =
			(const elf64_rela_t *)(image->file + sh->sh_offset);
		size_t count = (size_t)(sh->sh_size / sh->sh_entsize);
		uint8_t *target = image->sections[sh->sh_info];
		for (size_t j = 0; j < count; j++) {
			int r =
				elf_apply_rela(image, &relas[j], target, resolve, resolve_ctx);
			if (r != VFS_OK) {
				log_err(
					"elf", "relocation failed symbol=%s status=%d",
					elf_symbol_name(
						image, &image->symtab[ELF64_R_SYM(relas[j].r_info)]),
					r);
				return r;
			}
		}
	}
	return VFS_OK;
}

int elf_load_relocatable(elf_image_t *image, uint8_t *file, size_t file_size,
						 elf_alloc_section_t alloc, void *alloc_ctx,
						 elf_resolve_symbol_t resolve, void *resolve_ctx)
{
	if (!image || !file || !alloc)
		return VFS_ERR_INVAL;

	memset(image, 0, sizeof(*image));
	image->file = file;
	image->file_size = file_size;

	const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)file;
	int r = elf_validate(ehdr, file_size);
	if (r == VFS_OK)
		r = elf_load_sections(image, ehdr, alloc, alloc_ctx);
	if (r == VFS_OK)
		r = elf_find_symbols(image);
	if (r == VFS_OK)
		r = elf_apply_relocations(image, resolve, resolve_ctx);
	return r;
}

int elf_find_symbol_value(const elf_image_t *image, const char *name,
						  uint64_t *out, elf_resolve_symbol_t resolve,
						  void *resolve_ctx)
{
	if (!image || !name || !out)
		return VFS_ERR_INVAL;
	for (size_t i = 0; i < image->sym_count; i++) {
		if (strcmp(elf_symbol_name(image, &image->symtab[i]), name) != 0)
			continue;
		return elf_symbol_value(image, i, out, resolve, resolve_ctx);
	}
	return VFS_ERR_NOENT;
}

int elf_find_defined_symbol_value(const elf_image_t *image, const char *name,
								  uint64_t *out)
{
	if (!image || !name || !out)
		return VFS_ERR_INVAL;
	for (size_t i = 0; i < image->sym_count; i++) {
		const elf64_sym_t *sym = &image->symtab[i];
		if (sym->st_shndx == ELF_SHN_UNDEF)
			continue;
		if (strcmp(elf_symbol_name(image, sym), name) != 0)
			continue;
		return elf_symbol_value(image, i, out, NULL, NULL);
	}
	return VFS_ERR_NOENT;
}
