#include <lib/elf.h>
#include <debug/log.h>
#include <fs/vfs.h>
#include <lib/align.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/page.h>
#include <mm/paging.h>
#include <mm/pfndb.h>
#include <mm/vmm.h>

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_REL 1
#define ET_EXEC 2
#define ET_DYN 3
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_INTERP 3
#define PT_PHDR 6
#define PF_X 0x1
#define PF_W 0x2
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

#define ELF_ET_DYN_BASE 0x0000000040000000ULL
#define ELF_INTERP_BASE 0x0000000050000000ULL
#define ELF_STACK_RANDOM_SIZE 16

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
		return -EINVAL;
	if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
		ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3 ||
		ehdr->e_ident[4] != ELFCLASS64 || ehdr->e_ident[5] != ELFDATA2LSB)
		return -EINVAL;
	if (ehdr->e_type != ET_REL || ehdr->e_machine != EM_X86_64)
		return -EINVAL;
	if (ehdr->e_shentsize != sizeof(elf64_shdr_t) || ehdr->e_shnum == 0 ||
		!range_ok(file_size, ehdr->e_shoff,
				  (uint64_t)ehdr->e_shentsize * ehdr->e_shnum))
		return -EINVAL;
	return 0;
}

static int elf_validate_executable(const elf64_ehdr_t *ehdr, size_t file_size)
{
	if (file_size < sizeof(*ehdr))
		return -EINVAL;
	if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
		ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3 ||
		ehdr->e_ident[4] != ELFCLASS64 || ehdr->e_ident[5] != ELFDATA2LSB)
		return -EINVAL;
	if ((ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) ||
		ehdr->e_machine != EM_X86_64)
		return -EINVAL;
	if (ehdr->e_phentsize != sizeof(elf64_phdr_t) || ehdr->e_phnum == 0 ||
		!range_ok(file_size, ehdr->e_phoff,
				  (uint64_t)ehdr->e_phentsize * ehdr->e_phnum))
		return -EINVAL;
	return 0;
}

static int elf_load_sections(elf_image_t *image, const elf64_ehdr_t *ehdr,
							 elf_alloc_section_t alloc, void *alloc_ctx)
{
	image->section_count = ehdr->e_shnum;
	image->sections_hdr = (const elf64_shdr_t *)(image->file + ehdr->e_shoff);
	image->sections = kzalloc(image->section_count * sizeof(void *));
	if (!image->sections)
		return -ENOMEM;

	for (size_t i = 0; i < image->section_count; i++) {
		const elf64_shdr_t *sh = &image->sections_hdr[i];
		if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0)
			continue;

		uint64_t align = sh->sh_addralign ? sh->sh_addralign : PAGE_SIZE;
		uint8_t *mem = alloc ? alloc(sh->sh_size, align, alloc_ctx) : NULL;
		if (!mem)
			return -ENOMEM;
		if (sh->sh_type == SHT_NOBITS) {
			memset(mem, 0, (size_t)sh->sh_size);
		} else {
			if (!range_ok(image->file_size, sh->sh_offset, sh->sh_size))
				return -EINVAL;
			memcpy(mem, image->file + sh->sh_offset, (size_t)sh->sh_size);
		}
		image->sections[i] = mem;
	}
	return 0;
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
			return -EINVAL;

		const elf64_shdr_t *str = &image->sections_hdr[sh->sh_link];
		if (!range_ok(image->file_size, str->sh_offset, str->sh_size))
			return -EINVAL;
		image->symtab = (const elf64_sym_t *)(image->file + sh->sh_offset);
		image->sym_count = (size_t)(sh->sh_size / sh->sh_entsize);
		image->strtab = (const char *)(image->file + str->sh_offset);
		return 0;
	}
	return -EINVAL;
}

int elf_symbol_value(const elf_image_t *image, size_t index, uint64_t *out,
					 elf_resolve_symbol_t resolve, void *resolve_ctx)
{
	if (!image || !out || index >= image->sym_count)
		return -EINVAL;

	const elf64_sym_t *sym = &image->symtab[index];
	if (sym->st_shndx == ELF_SHN_UNDEF) {
		if (!resolve)
			return -ENOENT;
		return resolve(elf_symbol_name(image, sym), out, resolve_ctx);
	}
	if (sym->st_shndx >= image->section_count ||
		!image->sections[sym->st_shndx])
		return -EINVAL;
	*out = (uint64_t)image->sections[sym->st_shndx] + sym->st_value;
	return 0;
}

static int elf_apply_rela(elf_image_t *image, const elf64_rela_t *rela,
						  uint8_t *target, elf_resolve_symbol_t resolve,
						  void *resolve_ctx)
{
	uint64_t s = 0;
	int r = elf_symbol_value(image, (size_t)ELF64_R_SYM(rela->r_info), &s,
							 resolve, resolve_ctx);
	if (r != 0)
		return r;

	uint64_t p = (uint64_t)(target + rela->r_offset);
	uint64_t v = s + (uint64_t)rela->r_addend;
	switch (ELF64_R_TYPE(rela->r_info)) {
	case R_X86_64_64:
		*(uint64_t *)p = v;
		return 0;
	case R_X86_64_PC32:
	case R_X86_64_PLT32:
		*(uint32_t *)p = (uint32_t)(v - p);
		return 0;
	case R_X86_64_32:
	case R_X86_64_32S:
		*(uint32_t *)p = (uint32_t)v;
		return 0;
	default:
		log_err("elf", "unsupported relocation type=%u",
				ELF64_R_TYPE(rela->r_info));
		return -ENOSYS;
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
			return -EINVAL;
		if (!(image->sections_hdr[sh->sh_info].sh_flags & SHF_ALLOC))
			continue;
		if (sh->sh_entsize != sizeof(elf64_rela_t) ||
			!image->sections[sh->sh_info] ||
			!range_ok(image->file_size, sh->sh_offset, sh->sh_size))
			return -EINVAL;

		const elf64_rela_t *relas =
			(const elf64_rela_t *)(image->file + sh->sh_offset);
		size_t count = (size_t)(sh->sh_size / sh->sh_entsize);
		uint8_t *target = image->sections[sh->sh_info];
		for (size_t j = 0; j < count; j++) {
			int r =
				elf_apply_rela(image, &relas[j], target, resolve, resolve_ctx);
			if (r != 0) {
				log_err(
					"elf", "relocation failed symbol=%s status=%d",
					elf_symbol_name(
						image, &image->symtab[ELF64_R_SYM(relas[j].r_info)]),
					r);
				return r;
			}
		}
	}
	return 0;
}

int elf_load_relocatable(elf_image_t *image, uint8_t *file, size_t file_size,
						 elf_alloc_section_t alloc, void *alloc_ctx,
						 elf_resolve_symbol_t resolve, void *resolve_ctx)
{
	if (!image || !file || !alloc)
		return -EINVAL;

	memset(image, 0, sizeof(*image));
	image->file = file;
	image->file_size = file_size;

	const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)file;
	int r = elf_validate(ehdr, file_size);
	if (r == 0)
		r = elf_load_sections(image, ehdr, alloc, alloc_ctx);
	if (r == 0)
		r = elf_find_symbols(image);
	if (r == 0)
		r = elf_apply_relocations(image, resolve, resolve_ctx);
	return r;
}

int elf_find_symbol_value(const elf_image_t *image, const char *name,
						  uint64_t *out, elf_resolve_symbol_t resolve,
						  void *resolve_ctx)
{
	if (!image || !name || !out)
		return -EINVAL;
	for (size_t i = 0; i < image->sym_count; i++) {
		if (strcmp(elf_symbol_name(image, &image->symtab[i]), name) != 0)
			continue;
		return elf_symbol_value(image, i, out, resolve, resolve_ctx);
	}
	return -ENOENT;
}

int elf_find_defined_symbol_value(const elf_image_t *image, const char *name,
								  uint64_t *out)
{
	if (!image || !name || !out)
		return -EINVAL;
	for (size_t i = 0; i < image->sym_count; i++) {
		const elf64_sym_t *sym = &image->symtab[i];
		if (sym->st_shndx == ELF_SHN_UNDEF)
			continue;
		if (strcmp(elf_symbol_name(image, sym), name) != 0)
			continue;
		return elf_symbol_value(image, i, out, NULL, NULL);
	}
	return -ENOENT;
}

static int elf_load_segment(vas_t *vas, const uint8_t *file, size_t file_size,
							const elf64_phdr_t *ph, uint64_t load_bias)
{
	if (ph->p_memsz < ph->p_filesz ||
		!range_ok(file_size, ph->p_offset, ph->p_filesz))
		return -EINVAL;
	if (ph->p_memsz == 0)
		return 0;

	uint64_t seg_start = load_bias + ph->p_vaddr;
	uint64_t seg_end = seg_start + ph->p_memsz;
	if (seg_start < VAS_USER_START || seg_end < seg_start ||
		seg_end > VAS_USER_END)
		return -EINVAL;

	uint64_t map_start = ALIGN_DOWN(seg_start, PAGE_SIZE);
	uint64_t map_end = ALIGN_UP(seg_end, PAGE_SIZE);
	uint64_t map_len = map_end - map_start;
	uint64_t flags = VMM_PRESENT | VMM_USER | VAD_FIXED;
	if (ph->p_flags & PF_W)
		flags |= VMM_WRITABLE;
	if (!(ph->p_flags & PF_X))
		flags |= VMM_NX;

	uint64_t mapped = vas_map_anon(vas, map_start, map_len, flags);
	if (mapped != map_start)
		return -ENOMEM;

	size_t copied = 0;
	while (copied < ph->p_filesz) {
		uint64_t va = seg_start + copied;
		uint64_t phys = get_phys(vas->pml4, va);
		if (!phys)
			return -EINVAL;

		size_t page_off = (size_t)(va & (PAGE_SIZE - 1));
		size_t chunk = PAGE_SIZE - page_off;
		if (chunk > ph->p_filesz - copied)
			chunk = ph->p_filesz - copied;

		void *dst = PHYS_TO_VIRT(phys);
		memcpy(dst, file + ph->p_offset + copied, chunk);
		copied += chunk;
	}

	return 0;
}

static int elf_read_file(const char *path, uint8_t **file_out, size_t *size_out)
{
	if (!path || !file_out || !size_out)
		return -EINVAL;

	vfs_file_t *fp = NULL;
	int r = vfs_open(path, VFS_O_RDONLY, 0, &vfs_root_cred, &fp);
	if (r != 0)
		return r;

	vfs_node_t *node = vfs_file_node(fp);
	if (!node) {
		vfs_close(fp);
		return -EINVAL;
	}

	if (!VFS_S_ISREG(node->mode) || node->size == 0 || node->size > SIZE_MAX) {
		vfs_close(fp);
		return -EINVAL;
	}

	uint8_t *file = kzalloc((size_t)node->size);
	if (!file) {
		vfs_close(fp);
		return -ENOMEM;
	}

	size_t size = (size_t)node->size;
	size_t done = 0;

	r = vfs_read(fp, file, size, &done);
	vfs_close(fp);

	if (r != 0 || done != size) {
		kfree(file);
		return r == 0 ? -EINVAL : r;
	}

	*file_out = file;
	*size_out = size;
	return 0;
}

static int elf_find_interp(const uint8_t *file, size_t file_size,
						   const elf64_ehdr_t *ehdr, char *path_out,
						   size_t path_len)
{
	if (!file || !ehdr || !path_out || !path_len)
		return -EINVAL;

	path_out[0] = '\0';
	const elf64_phdr_t *phdrs = (const elf64_phdr_t *)(file + ehdr->e_phoff);
	for (size_t i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type != PT_INTERP)
			continue;
		if (phdrs[i].p_filesz == 0 || phdrs[i].p_filesz > path_len ||
			!range_ok(file_size, phdrs[i].p_offset, phdrs[i].p_filesz))
			return -EINVAL;
		memcpy(path_out, file + phdrs[i].p_offset, (size_t)phdrs[i].p_filesz);
		path_out[path_len - 1] = '\0';
		return 0;
	}

	return 0;
}

static int elf_find_loaded_phdr(const elf64_ehdr_t *ehdr,
								const elf64_phdr_t *phdrs, uint64_t load_bias,
								uint64_t *phdr_out)
{
	if (!ehdr || !phdrs || !phdr_out)
		return -EINVAL;

	for (size_t i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type == PT_PHDR) {
			*phdr_out = load_bias + phdrs[i].p_vaddr;
			return 0;
		}
	}

	for (size_t i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type != PT_LOAD)
			continue;
		if (ehdr->e_phoff < phdrs[i].p_offset ||
			ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize >
				phdrs[i].p_offset + phdrs[i].p_filesz)
			continue;
		*phdr_out =
			load_bias + phdrs[i].p_vaddr + (ehdr->e_phoff - phdrs[i].p_offset);
		return 0;
	}

	return -EINVAL;
}

static int elf_load_user_image_file(vas_t *vas, const char *path,
									const uint8_t *file, size_t file_size,
									uint64_t base_hint,
									elf_user_image_t *image_out,
									int as_interpreter)
{
	if (!vas || !path || !file || !image_out)
		return -EINVAL;

	const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)file;
	int r = elf_validate_executable(ehdr, file_size);
	if (r != 0)
		return r;

	const elf64_phdr_t *phdrs = (const elf64_phdr_t *)(file + ehdr->e_phoff);
	uint64_t load_bias = ehdr->e_type == ET_DYN ? base_hint : 0;
	for (size_t i = 0; i < ehdr->e_phnum; i++) {
		if (phdrs[i].p_type != PT_LOAD)
			continue;
		r = elf_load_segment(vas, file, file_size, &phdrs[i], load_bias);
		if (r != 0)
			return r;
	}

	uint64_t phdr_addr = 0;
	r = elf_find_loaded_phdr(ehdr, phdrs, load_bias, &phdr_addr);
	if (r != 0 && ehdr->e_type != ET_EXEC)
		return r;
	if (r != 0)
		phdr_addr = 0;

	memset(image_out, 0, sizeof(*image_out));
	image_out->entry = load_bias + ehdr->e_entry;
	if (!as_interpreter) {
		image_out->program_entry = load_bias + ehdr->e_entry;
		image_out->program_phdr = phdr_addr;
		image_out->program_phentsize = ehdr->e_phentsize;
		image_out->program_phnum = ehdr->e_phnum;
		size_t path_len = strlen(path);
		if (path_len >= sizeof(image_out->exec_path))
			path_len = sizeof(image_out->exec_path) - 1;
		memcpy(image_out->exec_path, path, path_len);
		image_out->exec_path[sizeof(image_out->exec_path) - 1] = '\0';
	} else {
		image_out->interp_base = load_bias;
	}

	return 0;
}

int elf_load_user_executable(vas_t *vas, const char *path,
							 elf_user_image_t *image_out)
{
	if (!vas || !path || !image_out)
		return -EINVAL;

	uint8_t *file = NULL;
	size_t file_size = 0;
	int r = elf_read_file(path, &file, &file_size);
	if (r != 0)
		return r;

	const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)file;
	char interp_path[256];
	r = elf_find_interp(file, file_size, ehdr, interp_path,
						sizeof(interp_path));
	if (r == 0)
		r = elf_load_user_image_file(vas, path, file, file_size,
									 ELF_ET_DYN_BASE, image_out, 0);
	kfree(file);
	if (r != 0)
		return r;

	if (!interp_path[0]) {
		image_out->entry = image_out->program_entry;
		return 0;
	}

	uint8_t *interp_file = NULL;
	size_t interp_size = 0;
	r = elf_read_file(interp_path, &interp_file, &interp_size);
	if (r != 0)
		return r;

	elf_user_image_t interp_image;
	r = elf_load_user_image_file(vas, interp_path, interp_file, interp_size,
								 ELF_INTERP_BASE, &interp_image, 1);
	kfree(interp_file);
	if (r != 0)
		return r;

	image_out->entry = interp_image.entry;
	image_out->interp_base = interp_image.interp_base;
	return 0;
}

static int stack_write_bytes(vas_t *vas, uint64_t *sp, const void *src,
							 size_t len, uint64_t align, uint64_t *addr_out)
{
	if (!vas || !sp || (len && !src))
		return -EINVAL;

	uint64_t next = *sp - len;
	if (align > 1)
		next &= ~(align - 1);
	if (next < VAS_USER_START)
		return -ENOMEM;

	uint64_t written = next;
	size_t copied = 0;
	while (copied < len) {
		uint64_t va = written + copied;
		uint64_t phys = get_phys(vas->pml4, va);
		if (!phys)
			return -EINVAL;

		size_t page_off = (size_t)(va & (PAGE_SIZE - 1));
		size_t chunk = PAGE_SIZE - page_off;
		if (chunk > len - copied)
			chunk = len - copied;

		memcpy(PHYS_TO_VIRT(phys), (const uint8_t *)src + copied, chunk);
		copied += chunk;
	}

	*sp = next;
	if (addr_out)
		*addr_out = written;
	return 0;
}

int elf_build_initial_stack(vas_t *vas, uint64_t stack_top,
							const char *exec_path, const char *const *argv,
							size_t argc, const char *const *envp, size_t envc,
							const elf_user_image_t *image, uint64_t *rsp_out)
{
	if (!vas || !image || !rsp_out || (!exec_path && argc))
		return -EINVAL;

	uint64_t sp = stack_top;
	uint64_t arg_ptrs[32];
	uint64_t env_ptrs[32];
	if (argc > 32 || envc > 32)
		return -EINVAL;

	for (size_t i = argc; i > 0; i--) {
		const char *s = argv[i - 1] ? argv[i - 1] : "";
		size_t len = strlen(s) + 1;
		int r = stack_write_bytes(vas, &sp, s, len, 1, &arg_ptrs[i - 1]);
		if (r != 0)
			return r;
	}

	for (size_t i = envc; i > 0; i--) {
		const char *s = envp[i - 1] ? envp[i - 1] : "";
		size_t len = strlen(s) + 1;
		int r = stack_write_bytes(vas, &sp, s, len, 1, &env_ptrs[i - 1]);
		if (r != 0)
			return r;
	}

	uint64_t execfn_ptr = 0;
	size_t execfn_len = strlen(exec_path ? exec_path : "") + 1;
	int r = stack_write_bytes(vas, &sp, exec_path ? exec_path : "", execfn_len,
							  1, &execfn_ptr);
	if (r != 0)
		return r;

	static const char platform[] = "x86_64";
	uint64_t platform_ptr = 0;
	r = stack_write_bytes(vas, &sp, platform, sizeof(platform), 1,
						  &platform_ptr);
	if (r != 0)
		return r;

	uint8_t random_bytes[ELF_STACK_RANDOM_SIZE] = {
		0x42, 0x61, 0x73, 0x65, 0x54, 0x6c, 0x73, 0x21,
		0x50, 0x69, 0x65, 0x45, 0x78, 0x65, 0x63, 0x00,
	};
	uint64_t random_ptr = 0;
	r = stack_write_bytes(vas, &sp, random_bytes, sizeof(random_bytes), 16,
						  &random_ptr);
	if (r != 0)
		return r;

	uint64_t auxv[] = {
		ELF_AUX_AT_PHDR,	 image->program_phdr,
		ELF_AUX_AT_PHENT,	 image->program_phentsize,
		ELF_AUX_AT_PHNUM,	 image->program_phnum,
		ELF_AUX_AT_PAGESZ,	 PAGE_SIZE,
		ELF_AUX_AT_BASE,	 image->interp_base,
		ELF_AUX_AT_FLAGS,	 0,
		ELF_AUX_AT_ENTRY,	 image->program_entry,
		ELF_AUX_AT_UID,		 0,
		ELF_AUX_AT_EUID,	 0,
		ELF_AUX_AT_GID,		 0,
		ELF_AUX_AT_EGID,	 0,
		ELF_AUX_AT_PLATFORM, platform_ptr,
		ELF_AUX_AT_HWCAP,	 0,
		ELF_AUX_AT_CLKTCK,	 100,
		ELF_AUX_AT_SECURE,	 0,
		ELF_AUX_AT_RANDOM,	 random_ptr,
		ELF_AUX_AT_EXECFN,	 execfn_ptr,
		ELF_AUX_AT_NULL,	 0,
	};

	size_t ptr_count = 1 + argc + 1 + envc + 1;
	size_t words = ptr_count + (sizeof(auxv) / sizeof(auxv[0]));
	sp &= ~0xFULL;
	if (words & 1)
		sp -= 8;
	uint64_t *stack_words = kzalloc(words * sizeof(uint64_t));
	if (!stack_words)
		return -ENOMEM;
	size_t idx = 0;
	stack_words[idx++] = argc;
	for (size_t i = 0; i < argc; i++)
		stack_words[idx++] = arg_ptrs[i];
	stack_words[idx++] = 0;
	for (size_t i = 0; i < envc; i++)
		stack_words[idx++] = env_ptrs[i];
	stack_words[idx++] = 0;
	for (size_t i = 0; i < sizeof(auxv) / sizeof(auxv[0]); i++)
		stack_words[idx++] = auxv[i];

	r = stack_write_bytes(vas, &sp, stack_words, words * sizeof(uint64_t), 8,
						  NULL);
	kfree(stack_words);
	if (r != 0)
		return r;

	*rsp_out = sp;
	return 0;
}