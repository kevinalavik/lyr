#include <fs/procfs.h>
#include <cpu/instr.h>
#include <dev/time.h>
#include <debug/log.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/page.h>
#include <mm/pmm.h>
#include <mm/heap.h>
#include <sched/sched.h>
#include <sys/smp.h>

extern int npf_snprintf_(char *buffer, size_t bufsz, const char *format, ...);

#define PROCFS_DEV 0x70726f63ULL
#ifndef LYR_VERSION
#define LYR_VERSION "unknown"
#endif

typedef enum procfs_kind {
	PROCFS_ROOT,
	PROCFS_PID_DIR,
	PROCFS_FILE_CPUINFO,
	PROCFS_FILE_MEMINFO,
	PROCFS_FILE_MOUNTS,
	PROCFS_FILE_UPTIME,
	PROCFS_FILE_VERSION,
	PROCFS_FILE_LOADAVG,
	PROCFS_FILE_STAT_GLOBAL,
	PROCFS_FILE_STATUS,
	PROCFS_FILE_STAT,
	PROCFS_FILE_CMDLINE,
	PROCFS_FILE_COMM,
	PROCFS_FILE_CWD,
} procfs_kind_t;

typedef struct procfs_node {
	vfs_node_t vnode;
	procfs_kind_t kind;
	pid_t pid;
} procfs_node_t;

typedef struct procfs_mount_record {
	char source[128];
	char target[256];
	char fstype[32];
	char opts[64];
	struct procfs_mount_record *next;
} procfs_mount_record_t;

static procfs_mount_record_t *procfs_mounts;
static char *procfs_cpuinfo_cache;
static size_t procfs_cpuinfo_cache_len;
static uint32_t procfs_cpuinfo_cache_cpus;

static int procfs_lookup(vfs_node_t *dir, const char *name, size_t len,
						 vfs_node_t **out);
static int procfs_read(vfs_node_t *node, uint64_t off, void *buf, size_t len,
					   size_t *done);
static int procfs_readdir(vfs_node_t *dir, size_t index, vfs_dirent_t *out);
static void procfs_release(vfs_node_t *node);
static void procfs_build_cpuinfo_cache(void);

static const vfs_ops_t procfs_ops = {
	.lookup = procfs_lookup,
	.read = procfs_read,
	.readdir = procfs_readdir,
	.release = procfs_release,
};

static procfs_node_t *procfs_to_node(vfs_node_t *node)
{
	return (procfs_node_t *)node;
}

static int name_eq(const char *name, size_t len, const char *lit)
{
	return strlen(lit) == len && memcmp(name, lit, len) == 0;
}

static int parse_pid(const char *name, size_t len, pid_t *out)
{
	if (!name || !len || !out)
		return 0;

	pid_t pid = 0;
	for (size_t i = 0; i < len; i++) {
		char c = name[i];
		if (c < '0' || c > '9')
			return 0;
		if (pid > (INT32_MAX - (c - '0')) / 10)
			return 0;
		pid = (pid_t)(pid * 10 + (c - '0'));
	}

	*out = pid;
	return 1;
}

static void pid_to_name(pid_t pid, char out[VFS_NAME_MAX + 1])
{
	char tmp[16];
	size_t n = 0;

	if (pid == 0) {
		out[0] = '0';
		out[1] = '\0';
		return;
	}

	while (pid > 0 && n < sizeof(tmp)) {
		tmp[n++] = (char)('0' + (pid % 10));
		pid /= 10;
	}

	for (size_t i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	out[n] = '\0';
}

static pid_t effective_pid(pid_t pid)
{
	if (pid >= 0)
		return pid;

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return -1;

	return thread->process->pid;
}

static int proc_info_for(pid_t pid, sched_process_info_t *info)
{
	pid = effective_pid(pid);
	if (pid < 0)
		return 0;
	return sched_process_get_info(pid, info);
}

static procfs_node_t *alloc_node(procfs_kind_t kind, pid_t pid, vfs_mode_t mode)
{
	procfs_node_t *node = kzalloc(sizeof(*node));
	if (!node)
		return NULL;

	vfs_node_init(&node->vnode, &procfs_ops, mode, 0, 0);
	node->vnode.dev = PROCFS_DEV;
	node->vnode.private_data = node;
	node->kind = kind;
	node->pid = pid;

	if (VFS_S_ISDIR(mode))
		node->vnode.nlink = 2;
	else
		node->vnode.nlink = 1;

	return node;
}

vfs_node_t *procfs_create_root(void)
{
	procfs_node_t *root = alloc_node(PROCFS_ROOT, -1, VFS_S_IFDIR | 0555);
	if (!root)
		return NULL;
	log_debug("procfs", "created root");
	return &root->vnode;
}

int procfs_mount(const char *target)
{
	vfs_node_t *root = procfs_create_root();
	if (!root)
		return -ENOMEM;

	int r = vfs_mount(target, root, &vfs_root_cred);
	vfs_node_release(root);
	if (r == 0)
		(void)procfs_note_mount("proc", target, "proc", "rw");
	return r;
}

static int lookup_pid_root(const char *name, size_t len, vfs_node_t **out)
{
	pid_t pid;

	if (name_eq(name, len, "self")) {
		pid = effective_pid(-1);
		if (pid < 0)
			return -ENOENT;
	} else if (!parse_pid(name, len, &pid)) {
		return -ENOENT;
	}

	sched_process_info_t info;
	if (!sched_process_get_info(pid, &info))
		return -ENOENT;

	procfs_node_t *node = alloc_node(PROCFS_PID_DIR, pid, VFS_S_IFDIR | 0555);
	if (!node)
		return -ENOMEM;

	*out = &node->vnode;
	return 0;
}

static int lookup_root_file(const char *name, size_t len, vfs_node_t **out)
{
	procfs_kind_t kind;

	if (name_eq(name, len, "cpuinfo"))
		kind = PROCFS_FILE_CPUINFO;
	else if (name_eq(name, len, "meminfo"))
		kind = PROCFS_FILE_MEMINFO;
	else if (name_eq(name, len, "mounts"))
		kind = PROCFS_FILE_MOUNTS;
	else if (name_eq(name, len, "uptime"))
		kind = PROCFS_FILE_UPTIME;
	else if (name_eq(name, len, "version"))
		kind = PROCFS_FILE_VERSION;
	else if (name_eq(name, len, "loadavg"))
		kind = PROCFS_FILE_LOADAVG;
	else if (name_eq(name, len, "stat"))
		kind = PROCFS_FILE_STAT_GLOBAL;
	else
		return -ENOENT;

	procfs_node_t *node = alloc_node(kind, -1, VFS_S_IFREG | 0444);
	if (!node)
		return -ENOMEM;
	*out = &node->vnode;
	return 0;
}

static int lookup_pid_file(procfs_node_t *dir, const char *name, size_t len,
						   vfs_node_t **out)
{
	procfs_kind_t kind;

	if (name_eq(name, len, "status"))
		kind = PROCFS_FILE_STATUS;
	else if (name_eq(name, len, "stat"))
		kind = PROCFS_FILE_STAT;
	else if (name_eq(name, len, "cmdline"))
		kind = PROCFS_FILE_CMDLINE;
	else if (name_eq(name, len, "comm"))
		kind = PROCFS_FILE_COMM;
	else if (name_eq(name, len, "cwd"))
		kind = PROCFS_FILE_CWD;
	else
		return -ENOENT;

	sched_process_info_t info;
	if (!proc_info_for(dir->pid, &info))
		return -ENOENT;

	procfs_node_t *node = alloc_node(kind, dir->pid, VFS_S_IFREG | 0444);
	if (!node)
		return -ENOMEM;

	*out = &node->vnode;
	return 0;
}

static int procfs_lookup(vfs_node_t *dir_node, const char *name, size_t len,
						 vfs_node_t **out)
{
	if (!dir_node || !name || !out)
		return -EINVAL;
	if (!VFS_S_ISDIR(dir_node->mode))
		return -ENOTDIR;

	procfs_node_t *dir = procfs_to_node(dir_node);

	if (len == 1 && name[0] == '.') {
		vfs_node_ref(dir_node);
		*out = dir_node;
		return 0;
	}
	if (len == 2 && name[0] == '.' && name[1] == '.') {
		if (dir->kind == PROCFS_ROOT) {
			vfs_node_ref(dir_node);
			*out = dir_node;
			return 0;
		}
		procfs_node_t *root = alloc_node(PROCFS_ROOT, -1, VFS_S_IFDIR | 0555);
		if (!root)
			return -ENOMEM;
		*out = &root->vnode;
		return 0;
	}

	switch (dir->kind) {
	case PROCFS_ROOT:
	{
		int r = lookup_root_file(name, len, out);
		if (r != -ENOENT)
			return r;
		return lookup_pid_root(name, len, out);
	}
	case PROCFS_PID_DIR:
		return lookup_pid_file(dir, name, len, out);
	default:
		return -ENOTDIR;
	}
}

static void dirent_fill(vfs_dirent_t *out, const char *name, vfs_mode_t mode)
{
	size_t len = strlen(name);
	if (len > VFS_NAME_MAX)
		len = VFS_NAME_MAX;
	memcpy(out->name, name, len);
	out->name[len] = '\0';
	out->mode = mode;
	out->uid = 0;
	out->gid = 0;
	out->size = 0;
	out->nlink = VFS_S_ISDIR(mode) ? 2 : 1;
}

static int procfs_root_readdir(size_t index, vfs_dirent_t *out)
{
	if (index == 0) {
		dirent_fill(out, ".", VFS_S_IFDIR | 0555);
		return 0;
	}
	if (index == 1) {
		dirent_fill(out, "..", VFS_S_IFDIR | 0555);
		return 0;
	}
	if (index == 2) {
		dirent_fill(out, "self", VFS_S_IFDIR | 0555);
		return 0;
	}
	if (index == 3) {
		dirent_fill(out, "cpuinfo", VFS_S_IFREG | 0444);
		return 0;
	}
	if (index == 4) {
		dirent_fill(out, "meminfo", VFS_S_IFREG | 0444);
		return 0;
	}
	if (index == 5) {
		dirent_fill(out, "mounts", VFS_S_IFREG | 0444);
		return 0;
	}
	if (index == 6) {
		dirent_fill(out, "uptime", VFS_S_IFREG | 0444);
		return 0;
	}
	if (index == 7) {
		dirent_fill(out, "version", VFS_S_IFREG | 0444);
		return 0;
	}
	if (index == 8) {
		dirent_fill(out, "loadavg", VFS_S_IFREG | 0444);
		return 0;
	}
	if (index == 9) {
		dirent_fill(out, "stat", VFS_S_IFREG | 0444);
		return 0;
	}

	sched_process_info_t info;
	if (!sched_process_get_nth(index - 10, &info))
		return -ENOENT;

	char name[VFS_NAME_MAX + 1];
	pid_to_name(info.pid, name);
	dirent_fill(out, name, VFS_S_IFDIR | 0555);
	return 0;
}

static int procfs_pid_readdir(procfs_node_t *dir, size_t index,
						  vfs_dirent_t *out)
{
	sched_process_info_t info;
	if (!proc_info_for(dir->pid, &info))
		return -ENOENT;

	switch (index) {
	case 0:
		dirent_fill(out, ".", VFS_S_IFDIR | 0555);
		return 0;
	case 1:
		dirent_fill(out, "..", VFS_S_IFDIR | 0555);
		return 0;
	case 2:
		dirent_fill(out, "status", VFS_S_IFREG | 0444);
		return 0;
	case 3:
		dirent_fill(out, "stat", VFS_S_IFREG | 0444);
		return 0;
	case 4:
		dirent_fill(out, "cmdline", VFS_S_IFREG | 0444);
		return 0;
	case 5:
		dirent_fill(out, "comm", VFS_S_IFREG | 0444);
		return 0;
	case 6:
		dirent_fill(out, "cwd", VFS_S_IFREG | 0444);
		return 0;
	default:
		return -ENOENT;
	}
}

static int procfs_readdir(vfs_node_t *dir_node, size_t index, vfs_dirent_t *out)
{
	if (!dir_node || !out)
		return -EINVAL;
	if (!VFS_S_ISDIR(dir_node->mode))
		return -ENOTDIR;

	procfs_node_t *dir = procfs_to_node(dir_node);
	if (dir->kind == PROCFS_ROOT)
		return procfs_root_readdir(index, out);
	if (dir->kind == PROCFS_PID_DIR)
		return procfs_pid_readdir(dir, index, out);
	return -ENOTDIR;
}

static char proc_state(const sched_process_info_t *info)
{
	if (info->zombie)
		return 'Z';
	if (info->dying)
		return 'X';
	return 'R';
}

static char proc_mode(const sched_process_info_t *info)
{
	return info->kernel ? 'K' : 'U';
}

static char proc_supervised(const sched_process_info_t *info)
{
	return info->supervised ? 'S' : '-';
}

static void procfs_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax,
						 uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
	uint32_t a = 0, b = 0, c = 0, d = 0;
	__asm__ volatile("cpuid"
					 : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
					 : "a"(leaf), "c"(subleaf));
	if (eax)
		*eax = a;
	if (ebx)
		*ebx = b;
	if (ecx)
		*ecx = c;
	if (edx)
		*edx = d;
}

static size_t procfs_copy_cstr_trim(char *dst, size_t cap, const char *src,
									 size_t src_len)
{
	size_t len = src_len;
	while (len > 0 && src[len - 1] == ' ')
		len--;
	if (cap == 0)
		return 0;
	if (len >= cap)
		len = cap - 1;
	memcpy(dst, src, len);
	dst[len] = '\0';
	return len;
}

typedef struct procfs_cpu_flag {
	uint32_t leaf;
	uint32_t subleaf;
	char reg;
	uint8_t bit;
	const char *name;
} procfs_cpu_flag_t;

static const procfs_cpu_flag_t procfs_cpu_flags[] = {
	{ 1, 0, 'd', 0, "fpu" },		   { 1, 0, 'd', 1, "vme" },
	{ 1, 0, 'd', 2, "de" },		   { 1, 0, 'd', 3, "pse" },
	{ 1, 0, 'd', 4, "tsc" },		   { 1, 0, 'd', 5, "msr" },
	{ 1, 0, 'd', 6, "pae" },		   { 1, 0, 'd', 7, "mce" },
	{ 1, 0, 'd', 8, "cx8" },		   { 1, 0, 'd', 9, "apic" },
	{ 1, 0, 'd', 11, "sep" },		   { 1, 0, 'd', 12, "mtrr" },
	{ 1, 0, 'd', 13, "pge" },		   { 1, 0, 'd', 14, "mca" },
	{ 1, 0, 'd', 15, "cmov" },		   { 1, 0, 'd', 16, "pat" },
	{ 1, 0, 'd', 17, "pse36" },	   { 1, 0, 'd', 19, "clflush" },
	{ 1, 0, 'd', 23, "mmx" },		   { 1, 0, 'd', 24, "fxsr" },
	{ 1, 0, 'd', 25, "sse" },		   { 1, 0, 'd', 26, "sse2" },
	{ 1, 0, 'd', 28, "ht" },		   { 1, 0, 'c', 0, "sse3" },
	{ 1, 0, 'c', 1, "pclmulqdq" },	   { 1, 0, 'c', 9, "ssse3" },
	{ 1, 0, 'c', 12, "fma" },		   { 1, 0, 'c', 13, "cx16" },
	{ 1, 0, 'c', 19, "sse4_1" },	   { 1, 0, 'c', 20, "sse4_2" },
	{ 1, 0, 'c', 22, "movbe" },	   { 1, 0, 'c', 23, "popcnt" },
	{ 1, 0, 'c', 25, "aes" },		   { 1, 0, 'c', 26, "xsave" },
	{ 1, 0, 'c', 28, "avx" },		   { 1, 0, 'c', 29, "f16c" },
	{ 1, 0, 'c', 30, "rdrand" },	   { 7, 0, 'b', 0, "fsgsbase" },
	{ 7, 0, 'b', 3, "bmi1" },		   { 7, 0, 'b', 5, "avx2" },
	{ 7, 0, 'b', 7, "smep" },		   { 7, 0, 'b', 8, "bmi2" },
	{ 7, 0, 'b', 18, "rdseed" },	   { 7, 0, 'b', 19, "adx" },
	{ 7, 0, 'b', 20, "smap" },		   { 7, 0, 'b', 23, "clflushopt" },
	{ 7, 0, 'b', 29, "sha_ni" },	   { 0x80000001u, 0, 'd', 11, "syscall" },
	{ 0x80000001u, 0, 'd', 20, "nx" }, { 0x80000001u, 0, 'd', 26, "pdpe1gb" },
	{ 0x80000001u, 0, 'd', 27, "rdtscp" },
	{ 0x80000001u, 0, 'd', 29, "lm" }, { 0x80000001u, 0, 'c', 0, "lahf_lm" },
	{ 0x80000001u, 0, 'c', 5, "abm" }, { 0x80000001u, 0, 'c', 6, "sse4a" },
	{ 0x80000001u, 0, 'c', 2, "svm" },
};

static size_t procfs_append_flags(char *buf, size_t size, uint32_t max_basic,
								  uint32_t max_ext, uint32_t leaf1_ecx,
								  uint32_t leaf1_edx, uint32_t leaf7_ebx,
								  uint32_t ext1_ecx, uint32_t ext1_edx)
{
	size_t pos = 0;
	int n = npf_snprintf(buf + pos, size - pos, "flags\t\t: ");
	if (n < 0 || (size_t)n >= size - pos)
		return size ? size - 1 : 0;
	pos += (size_t)n;

	for (size_t i = 0; i < sizeof(procfs_cpu_flags) / sizeof(procfs_cpu_flags[0]);
		 i++) {
		const procfs_cpu_flag_t *flag = &procfs_cpu_flags[i];
		uint32_t reg = 0;

		if (flag->leaf == 7 && max_basic < 7)
			continue;
		if (flag->leaf == 0x80000001u && max_ext < 0x80000001u)
			continue;

		if (flag->leaf == 1) {
			reg = flag->reg == 'c' ? leaf1_ecx : leaf1_edx;
		} else if (flag->leaf == 7) {
			reg = leaf7_ebx;
		} else if (flag->leaf == 0x80000001u) {
			reg = flag->reg == 'c' ? ext1_ecx : ext1_edx;
		}

		if (((reg >> flag->bit) & 1u) == 0)
			continue;

		n = npf_snprintf(buf + pos, size - pos, "%s%s", pos > 9 ? " " : "",
						 flag->name);
		if (n < 0 || (size_t)n >= size - pos)
			return size ? size - 1 : 0;
		pos += (size_t)n;
	}

	n = npf_snprintf(buf + pos, size - pos, "\n");
	if (n < 0 || (size_t)n >= size - pos)
		return size ? size - 1 : 0;
	pos += (size_t)n;
	return pos;
}

static size_t format_cpuinfo(char *buf, size_t size)
{
	uint32_t max_basic = 0, max_ext = 0;
	uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
	uint32_t leaf1_eax = 0, leaf1_ebx = 0, leaf1_ecx = 0, leaf1_edx = 0;
	uint32_t leaf7_ebx = 0;
	uint32_t ext1_ecx = 0, ext1_edx = 0;
	uint32_t ext6_ecx = 0;
	uint32_t ext8_eax = 0;
	char vendor[13] = "unknown";
	char model[49] = "unknown";
	unsigned family = 0, model_id = 0, stepping = 0;
	unsigned clflush_size = 0;
	unsigned cache_kb = 0;
	unsigned phys_bits = 0, virt_bits = 0;

	procfs_cpuid(0, 0, &max_basic, &ebx, &ecx, &edx);
	memcpy(vendor + 0, &ebx, 4);
	memcpy(vendor + 4, &edx, 4);
	memcpy(vendor + 8, &ecx, 4);
	vendor[12] = '\0';

	if (max_basic >= 1) {
		procfs_cpuid(1, 0, &leaf1_eax, &leaf1_ebx, &leaf1_ecx, &leaf1_edx);
		stepping = leaf1_eax & 0xfu;
		unsigned base_model = (leaf1_eax >> 4) & 0xfu;
		unsigned base_family = (leaf1_eax >> 8) & 0xfu;
		unsigned ext_model = (leaf1_eax >> 16) & 0xfu;
		unsigned ext_family = (leaf1_eax >> 20) & 0xffu;
		family = base_family == 0xfu ? base_family + ext_family : base_family;
		model_id = (base_family == 0x6u || base_family == 0xfu)
					   ? (base_model | (ext_model << 4))
					   : base_model;
		clflush_size = ((leaf1_ebx >> 8) & 0xffu) * 8u;
	}

	procfs_cpuid(0x80000000u, 0, &max_ext, NULL, NULL, NULL);
	if (max_ext >= 0x80000004u) {
		uint32_t brand[12];
		for (uint32_t i = 0; i < 3; i++) {
			procfs_cpuid(0x80000002u + i, 0, &brand[i * 4 + 0],
						 &brand[i * 4 + 1], &brand[i * 4 + 2],
						 &brand[i * 4 + 3]);
		}
		procfs_copy_cstr_trim(model, sizeof(model), (const char *)brand, 48);
	}
	if (max_basic >= 7)
		procfs_cpuid(7, 0, NULL, &leaf7_ebx, NULL, NULL);
	if (max_ext >= 0x80000001u)
		procfs_cpuid(0x80000001u, 0, NULL, NULL, &ext1_ecx, &ext1_edx);
	if (max_ext >= 0x80000006u) {
		procfs_cpuid(0x80000006u, 0, NULL, NULL, &ext6_ecx, NULL);
		cache_kb = (ext6_ecx >> 16) & 0xffffu;
	}
	if (max_ext >= 0x80000008u) {
		procfs_cpuid(0x80000008u, 0, &ext8_eax, NULL, NULL, NULL);
		phys_bits = ext8_eax & 0xffu;
		virt_bits = (ext8_eax >> 8) & 0xffu;
	}

	size_t pos = 0;

	for (uint32_t i = 0; i < cpu_count && pos < size; i++) {
		int n = npf_snprintf(buf + pos, size - pos,
							 "processor\t: %u\n"
							 "vendor_id\t: %s\n"
							 "cpu family\t: %u\n"
							 "model\t\t: %u\n"
							 "model name\t: %s\n"
							 "stepping\t: %u\n",
							 i, vendor, family, model_id, model, stepping);
		if (n < 0)
			break;
		if ((size_t)n >= size - pos)
			return size ? size - 1 : 0;
		pos += (size_t)n;

		n = npf_snprintf(buf + pos, size - pos, "siblings\t: %u\n", cpu_count);
		if (n < 0 || (size_t)n >= size - pos)
			return size ? size - 1 : 0;
		pos += (size_t)n;

		n = npf_snprintf(buf + pos, size - pos, "apicid\t\t: %u\n"
												 "initial apicid\t: %u\n",
						 cpu_locals[i].lapic_id, cpu_locals[i].lapic_id);
		if (n < 0 || (size_t)n >= size - pos)
			return size ? size - 1 : 0;
		pos += (size_t)n;

		n = npf_snprintf(buf + pos, size - pos, "fpu\t\t: %s\n"
												 "fpu_exception\t: %s\n"
												 "cpuid level\t: %u\n",
						 (leaf1_edx & 1u) ? "yes" : "no",
						 (leaf1_edx & 1u) ? "yes" : "no", max_basic);
		if (n < 0 || (size_t)n >= size - pos)
			return size ? size - 1 : 0;
		pos += (size_t)n;

		size_t added = procfs_append_flags(buf + pos, size - pos, max_basic,
										   max_ext, leaf1_ecx, leaf1_edx,
										   leaf7_ebx, ext1_ecx, ext1_edx);
		if (added >= size - pos)
			return size ? size - 1 : 0;
		pos += added;

		if (cache_kb != 0) {
			n = npf_snprintf(buf + pos, size - pos, "cache size\t: %u KB\n",
							 cache_kb);
			if (n < 0 || (size_t)n >= size - pos)
				return size ? size - 1 : 0;
			pos += (size_t)n;
		}

		if (clflush_size != 0) {
			n = npf_snprintf(buf + pos, size - pos, "clflush size\t: %u\n"
													 "cache_alignment\t: %u\n",
							 clflush_size, clflush_size);
			if (n < 0 || (size_t)n >= size - pos)
				return size ? size - 1 : 0;
			pos += (size_t)n;
		}

		if (phys_bits != 0 && virt_bits != 0) {
			n = npf_snprintf(buf + pos, size - pos,
							 "address sizes\t: %u bits physical, %u bits virtual\n",
							 phys_bits, virt_bits);
			if (n < 0 || (size_t)n >= size - pos)
				return size ? size - 1 : 0;
			pos += (size_t)n;
		}

		n = npf_snprintf(buf + pos, size - pos, "\n");
		if (n < 0 || (size_t)n >= size - pos)
			return size ? size - 1 : 0;
		pos += (size_t)n;
	}

	return pos;
}

static void procfs_build_cpuinfo_cache(void)
{
	if (procfs_cpuinfo_cache && procfs_cpuinfo_cache_cpus == cpu_count &&
		procfs_cpuinfo_cache_len != 0)
		return;

	if (procfs_cpuinfo_cache) {
		kfree(procfs_cpuinfo_cache);
		procfs_cpuinfo_cache = NULL;
		procfs_cpuinfo_cache_len = 0;
		procfs_cpuinfo_cache_cpus = 0;
	}

	size_t cap = (size_t)cpu_count * 2048;
	if (cap < 4096)
		cap = 4096;

	char *buf = kzalloc(cap);
	if (!buf)
		return;

	size_t len = format_cpuinfo(buf, cap);
	procfs_cpuinfo_cache = buf;
	procfs_cpuinfo_cache_len = len;
	procfs_cpuinfo_cache_cpus = cpu_count;
}

static size_t build_cpuinfo(char *buf, size_t size)
{
	procfs_build_cpuinfo_cache();
	if (!procfs_cpuinfo_cache || size == 0)
		return 0;

	size_t len = procfs_cpuinfo_cache_len;
	if (len >= size)
		len = size - 1;
	memcpy(buf, procfs_cpuinfo_cache, len);
	buf[len] = '\0';
	return len;
}

static size_t build_meminfo(char *buf, size_t size)
{
	uint64_t total_kb = (pmm_total_pages() * PAGE_SIZE) / 1024;
	uint64_t free_kb = (pmm_free_pages() * PAGE_SIZE) / 1024;
	uint64_t used_kb = total_kb >= free_kb ? total_kb - free_kb : 0;

	int n = npf_snprintf(
		buf, size,
		"MemTotal:\t%lu kB\nMemFree:\t%lu kB\nMemAvailable:\t%lu kB\nMemUsed:\t%lu kB\n",
		(unsigned long)total_kb, (unsigned long)free_kb,
		(unsigned long)free_kb, (unsigned long)used_kb);
	if (n < 0)
		return 0;
	if ((size_t)n >= size)
		return size ? size - 1 : 0;
	return (size_t)n;
}

static size_t build_mounts(char *buf, size_t size)
{
	size_t pos = 0;
	for (procfs_mount_record_t *mnt = procfs_mounts; mnt && pos < size;
		 mnt = mnt->next) {
		int n = npf_snprintf(buf + pos, size - pos, "%s %s %s %s 0 0\n",
							 mnt->source, mnt->target, mnt->fstype, mnt->opts);
		if (n < 0)
			break;
		if ((size_t)n >= size - pos)
			return size ? size - 1 : 0;
		pos += (size_t)n;
	}
	return pos;
}

static size_t build_uptime(char *buf, size_t size)
{
	uint64_t ns = time_monotonic_ns();
	uint64_t sec = ns / NSEC_PER_SEC;
	uint64_t frac = (ns % NSEC_PER_SEC) / 10000000ULL;
	int n = npf_snprintf(buf, size, "%lu.%02lu %lu.%02lu\n",
						 (unsigned long)sec, (unsigned long)frac,
						 (unsigned long)sec, (unsigned long)frac);
	if (n < 0)
		return 0;
	if ((size_t)n >= size)
		return size ? size - 1 : 0;
	return (size_t)n;
}

static size_t build_version(char *buf, size_t size)
{
	int n = npf_snprintf(buf, size, "Lyr version %s\n", LYR_VERSION);
	if (n < 0)
		return 0;
	if ((size_t)n >= size)
		return size ? size - 1 : 0;
	return (size_t)n;
}

static size_t build_loadavg(char *buf, size_t size)
{
	uint64_t loadavg[3] = { 0, 0, 0 };
	size_t runnable = 0;
	size_t total = 0;
	pid_t last_pid = sched_last_pid();
	sched_loadavg_task_counts(&runnable, &total);

	sched_loadavg_snapshot(loadavg);

	uint64_t l1_int = loadavg[0] / 1000000ULL;
	uint64_t l1_frac = (loadavg[0] % 1000000ULL) / 10000ULL;
	uint64_t l5_int = loadavg[1] / 1000000ULL;
	uint64_t l5_frac = (loadavg[1] % 1000000ULL) / 10000ULL;
	uint64_t l15_int = loadavg[2] / 1000000ULL;
	uint64_t l15_frac = (loadavg[2] % 1000000ULL) / 10000ULL;

	int n = npf_snprintf(buf, size,
						 "%lu.%02lu %lu.%02lu %lu.%02lu %zu/%zu %d\n",
						 (unsigned long)l1_int, (unsigned long)l1_frac,
						 (unsigned long)l5_int, (unsigned long)l5_frac,
						 (unsigned long)l15_int, (unsigned long)l15_frac,
						 runnable, total ? total : 1,
						 last_pid > 0 ? last_pid : 0);
	if (n < 0)
		return 0;
	if ((size_t)n >= size)
		return size ? size - 1 : 0;
	return (size_t)n;
}

static size_t build_global_stat(char *buf, size_t size)
{
	size_t processes = 0;
	sched_process_info_t info;
	while (sched_process_get_nth(processes, &info))
		processes++;

	uint64_t btime = 0;
	int64_t now_sec = 0;
	long now_nsec = 0;
	if (time_get(LYR_CLOCK_REALTIME, &now_sec, &now_nsec) == 0) {
		uint64_t uptime_sec = time_monotonic_ns() / NSEC_PER_SEC;
		if ((uint64_t)now_sec >= uptime_sec)
			btime = (uint64_t)now_sec - uptime_sec;
	}

	int n = npf_snprintf(buf, size,
						 "cpu  0 0 0 0 0 0 0 0 0 0\n"
						 "cpu0 0 0 0 0 0 0 0 0 0 0\n"
						 "intr 0\nctxt 0\nbtime %lu\nprocesses %zu\n"
						 "procs_running 1\nprocs_blocked 0\n",
						 (unsigned long)btime, processes);
	if (n < 0)
		return 0;
	if ((size_t)n >= size)
		return size ? size - 1 : 0;
	return (size_t)n;
}

static size_t build_status(const sched_process_info_t *info, char *buf,
					   size_t size)
{
	int n = npf_snprintf(
		buf, size,
		"Name:\t%s\nState:\t%c\nMode:\t%c\nSupervised:\t%c\nPid:\t%d\nPPid:\t%d\nThreads:\t%u\nUid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nCwd:\t%s\n",
		info->name, proc_state(info), proc_mode(info), proc_supervised(info),
		info->pid, info->ppid, info->thread_count, info->ruid, info->euid,
		info->ruid, info->euid, info->rgid, info->egid, info->rgid,
		info->egid, info->cwd);
	if (n < 0)
		return 0;
	if ((size_t)n >= size)
		return size ? size - 1 : 0;
	return (size_t)n;
}

static size_t build_stat(const sched_process_info_t *info, char *buf,
					 size_t size)
{
	int n = npf_snprintf(
		buf, size,
		"%d (%s) %c %d 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 %u 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 %d\n",
		info->pid, info->name, proc_state(info), info->ppid,
		info->thread_count, info->exit_status);
	if (n < 0)
		return 0;
	if ((size_t)n >= size)
		return size ? size - 1 : 0;
	return (size_t)n;
}

static size_t build_content(procfs_node_t *node, char *buf, size_t size)
{
	sched_process_info_t info;

	switch (node->kind) {
	case PROCFS_FILE_CPUINFO:
		return build_cpuinfo(buf, size);
	case PROCFS_FILE_MEMINFO:
		return build_meminfo(buf, size);
	case PROCFS_FILE_MOUNTS:
		return build_mounts(buf, size);
	case PROCFS_FILE_UPTIME:
		return build_uptime(buf, size);
	case PROCFS_FILE_VERSION:
		return build_version(buf, size);
	case PROCFS_FILE_LOADAVG:
		return build_loadavg(buf, size);
	case PROCFS_FILE_STAT_GLOBAL:
		return build_global_stat(buf, size);
	default:
		break;
	}

	if (!proc_info_for(node->pid, &info))
		return 0;

	switch (node->kind) {
	case PROCFS_FILE_STATUS:
		return build_status(&info, buf, size);
	case PROCFS_FILE_STAT:
		return build_stat(&info, buf, size);
	case PROCFS_FILE_CMDLINE: {
		size_t len = strlen(info.name);
		if (len + 1 > size)
			len = size ? size - 1 : 0;
		if (len)
			memcpy(buf, info.name, len);
		if (size)
			buf[len++] = '\0';
		return len;
	}
	case PROCFS_FILE_COMM: {
		int n = npf_snprintf(buf, size, "%s\n", info.name);
		if (n < 0)
			return 0;
		if ((size_t)n >= size)
			return size ? size - 1 : 0;
		return (size_t)n;
	}
	case PROCFS_FILE_CWD: {
		int n = npf_snprintf(buf, size, "%s\n", info.cwd);
		if (n < 0)
			return 0;
		if ((size_t)n >= size)
			return size ? size - 1 : 0;
		return (size_t)n;
	}
	default:
		return 0;
	}
}

static int procfs_read(vfs_node_t *node, uint64_t off, void *buf, size_t len,
					   size_t *done)
{
	if (done)
		*done = 0;
	if (!node || !buf)
		return -EINVAL;
	if (VFS_S_ISDIR(node->mode))
		return -EISDIR;

	char tmp[4096];
	procfs_node_t *pn = procfs_to_node(node);
	size_t total = build_content(pn, tmp, sizeof(tmp));

	if (!total && (pn->kind == PROCFS_FILE_STATUS ||
				   pn->kind == PROCFS_FILE_STAT ||
				   pn->kind == PROCFS_FILE_CMDLINE ||
				   pn->kind == PROCFS_FILE_COMM ||
				   pn->kind == PROCFS_FILE_CWD))
		return -ENOENT;

	if (off >= total || len == 0)
		return 0;

	size_t n = total - (size_t)off;
	if (n > len)
		n = len;
	memcpy(buf, tmp + off, n);
	if (done)
		*done = n;
	return 0;
}

static void procfs_release(vfs_node_t *node)
{
	kfree(node);
}

int procfs_note_mount(const char *source, const char *target, const char *fstype,
					  const char *opts)
{
	if (!source || !target || !fstype)
		return -EINVAL;

	for (procfs_mount_record_t *mnt = procfs_mounts; mnt; mnt = mnt->next) {
		if (strcmp(mnt->target, target) == 0) {
			npf_snprintf(mnt->source, sizeof(mnt->source), "%s", source);
			npf_snprintf(mnt->fstype, sizeof(mnt->fstype), "%s", fstype);
			npf_snprintf(mnt->opts, sizeof(mnt->opts), "%s",
						 opts ? opts : "rw");
			return 0;
		}
	}

	procfs_mount_record_t *mnt = kzalloc(sizeof(*mnt));
	if (!mnt)
		return -ENOMEM;

	npf_snprintf(mnt->source, sizeof(mnt->source), "%s", source);
	npf_snprintf(mnt->target, sizeof(mnt->target), "%s", target);
	npf_snprintf(mnt->fstype, sizeof(mnt->fstype), "%s", fstype);
	npf_snprintf(mnt->opts, sizeof(mnt->opts), "%s", opts ? opts : "rw");
	mnt->next = procfs_mounts;
	procfs_mounts = mnt;
	return 0;
}
