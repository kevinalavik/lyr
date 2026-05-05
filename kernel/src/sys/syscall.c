#include <sys/syscall.h>
#include <cpu/instr.h>
#include <debug/log.h>
#include <fs/mount.h>
#include <fs/vfs.h>
#include <init/init.h>
#include <lib/elf.h>
#include <lib/string.h>
#include <mm/vmm.h>
#include <sched/sched.h>
#include <util/kprintf.h>

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_SFMASK 0xC0000084
#define EFER_SCE (1ULL << 0)
#define RFLAGS_IF (1ULL << 9)
#define KERNEL_CS 0x08
#define USER_CS 0x1B
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define EXEC_ARG_MAX 32
#define EXEC_STR_MAX 256

extern void syscall_entry(void);

void syscall_init(void)
{
	uint64_t efer = rdmsr(MSR_EFER);
	uint64_t star = ((uint64_t)(KERNEL_CS & ~0x3) << 32) |
					((uint64_t)((USER_CS - 0x10) & ~0x3) << 48);
	wrmsr(MSR_STAR, star);
	wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
	wrmsr(MSR_SFMASK, RFLAGS_IF);
	wrmsr(MSR_EFER, efer | EFER_SCE);
}

typedef long (*syscall_handler_t)(interrupt_frame_t *frame);

typedef struct {
	char name[VFS_NAME_MAX + 1];
	vfs_mode_t mode;
	vfs_uid_t uid;
	vfs_gid_t gid;
	uint64_t size;
	uint32_t nlink;
} syscall_dirent_t;

typedef struct {
	vfs_mode_t mode;
	vfs_uid_t uid;
	vfs_gid_t gid;
	uint64_t size;
	uint32_t nlink;
} syscall_stat_t;

static int syscall_copy_user_string(uint64_t user, char *out, size_t out_len)
{
	if (!user || user >= VAS_USER_END || !out || out_len == 0)
		return VFS_ERR_INVAL;

	for (size_t i = 0; i < out_len; i++) {
		if (user + i >= VAS_USER_END)
			return VFS_ERR_INVAL;
		char c = ((const char *)user)[i];
		out[i] = c;
		if (c == '\0')
			return VFS_OK;
	}

	out[out_len - 1] = '\0';
	return VFS_ERR_NAMETOOLONG;
}

static int syscall_user_range_ok(uint64_t addr, size_t len)
{
	if (addr >= VAS_USER_END)
		return 0;
	return len <= VAS_USER_END - addr;
}

static int syscall_copy_user_ptrs(uint64_t user, uint64_t *out, size_t cap,
								  size_t *count_out)
{
	if (!out || !count_out)
		return VFS_ERR_INVAL;

	*count_out = 0;
	if (!user)
		return VFS_OK;

	for (size_t i = 0; i < cap; i++) {
		if (!syscall_user_range_ok(user + i * sizeof(uint64_t),
								   sizeof(uint64_t)))
			return VFS_ERR_INVAL;
		uint64_t ptr = ((const uint64_t *)user)[i];
		if (!ptr)
			return VFS_OK;
		out[i] = ptr;
		*count_out = i + 1;
	}

	return VFS_ERR_NAMETOOLONG;
}

static uint32_t syscall_mmap_prot_to_vmm(int prot)
{
	uint32_t flags = VMM_PRESENT | VMM_USER;
	if (prot & 0x2)
		flags |= VMM_WRITABLE;
	if (!(prot & 0x4))
		flags |= VMM_NX;
	return flags;
}

static long syscall_copy_stat_out(uint64_t user, const vfs_stat_t *st)
{
	if (!st || !syscall_user_range_ok(user, sizeof(syscall_stat_t)))
		return VFS_ERR_INVAL;

	syscall_stat_t out = {
		.mode = st->mode,
		.uid = st->uid,
		.gid = st->gid,
		.size = st->size,
		.nlink = st->nlink,
	};
	memcpy((void *)user, &out, sizeof(out));
	return VFS_OK;
}

static long sys_read_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	int fd = (int)frame->rdi;
	void *buf = (void *)frame->rsi;
	size_t len = (size_t)frame->rdx;
	if (fd < 0 || fd >= SCHED_FILE_MAX || !buf ||
		!syscall_user_range_ok((uint64_t)buf, len))
		return VFS_ERR_INVAL;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	size_t done = 0;
	int r = vfs_read(file, buf, len, &done);
	if (r != VFS_OK)
		return r;
	return (long)done;
}

static long sys_write_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	int fd = (int)frame->rdi;
	const char *buf = (const char *)frame->rsi;
	size_t len = (size_t)frame->rdx;

	if (fd < 0 || fd >= SCHED_FILE_MAX || !buf ||
		!syscall_user_range_ok((uint64_t)buf, len))
		return VFS_ERR_INVAL;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	size_t done = 0;
	int r = vfs_write(file, buf, len, &done);
	if (r != VFS_OK)
		return r;

	return (long)done;
}

static long sys_open_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	char path[256];
	int r = syscall_copy_user_string(frame->rdi, path, sizeof(path));
	if (r != VFS_OK)
		return r;

	for (int fd = 0; fd < SCHED_FILE_MAX; fd++) {
		if (thread->process->files[fd])
			continue;

		vfs_file_t *file = NULL;
		r = vfs_open(path, (uint32_t)frame->rsi, (vfs_mode_t)frame->rdx,
					 &vfs_root_cred, &file);
		if (r != VFS_OK)
			return r;

		thread->process->files[fd] = file;
		return fd;
	}

	return VFS_ERR_NOMEM;
}

static long sys_close_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	int fd = (int)frame->rdi;
	if (fd < 0 || fd >= SCHED_FILE_MAX || !thread->process->files[fd])
		return VFS_ERR_BADF;

	vfs_close(thread->process->files[fd]);
	thread->process->files[fd] = NULL;
	return VFS_OK;
}

static long sys_stat_handler(interrupt_frame_t *frame)
{
	char path[256];
	int r = syscall_copy_user_string(frame->rdi, path, sizeof(path));
	if (r != VFS_OK)
		return r;

	vfs_stat_t st;
	r = vfs_stat(path, &vfs_root_cred, &st);
	if (r != VFS_OK)
		return r;
	return syscall_copy_stat_out(frame->rsi, &st);
}

static long sys_lseek_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	int fd = (int)frame->rdi;
	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return VFS_ERR_BADF;
	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	uint64_t new_off = 0;
	int r = vfs_seek(file, (int)frame->rdx, (int64_t)frame->rsi, &new_off);
	if (r != VFS_OK)
		return r;
	return (long)new_off;
}

static long sys_access_handler(interrupt_frame_t *frame)
{
	char path[256];
	int r = syscall_copy_user_string(frame->rdi, path, sizeof(path));
	if (r != VFS_OK)
		return r;

	vfs_node_t *node = NULL;
	r = vfs_resolve(path, &vfs_root_cred, &node);
	if (r != VFS_OK)
		return r;
	r = vfs_access(node, &vfs_root_cred, (int)frame->rsi);
	vfs_node_release(node);
	return r;
}

static long sys_getdents_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	int fd = (int)frame->rdi;
	uint64_t user = frame->rsi;
	size_t cap = (size_t)frame->rdx;
	if (fd < 0 || fd >= SCHED_FILE_MAX || !syscall_user_range_ok(user, cap))
		return VFS_ERR_INVAL;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	size_t copied = 0;
	while (copied + sizeof(syscall_dirent_t) <= cap) {
		vfs_dirent_t ent;
		int r = vfs_readdir(file->node, (size_t)file->offset, &ent);
		if (r == VFS_ERR_NOENT)
			break;
		if (r != VFS_OK)
			return copied ? (long)copied : r;

		syscall_dirent_t out;
		memset(&out, 0, sizeof(out));
		memcpy(out.name, ent.name, sizeof(out.name) - 1);
		out.mode = ent.mode;
		out.uid = ent.uid;
		out.gid = ent.gid;
		out.size = ent.size;
		out.nlink = ent.nlink;
		memcpy((void *)(user + copied), &out, sizeof(out));
		copied += sizeof(out);
		file->offset++;
	}

	return (long)copied;
}

static long sys_chroot_handler(interrupt_frame_t *frame)
{
	char path[256];
	int r = syscall_copy_user_string(frame->rdi, path, sizeof(path));
	if (r != VFS_OK)
		return r;
	return vfs_chroot(path, &vfs_root_cred);
}

static long sys_mount_handler(interrupt_frame_t *frame)
{
	char source[256];
	char target[256];
	char fstype[32];
	int r = syscall_copy_user_string(frame->rdi, source, sizeof(source));
	if (r != VFS_OK)
		return r;
	r = syscall_copy_user_string(frame->rsi, target, sizeof(target));
	if (r != VFS_OK)
		return r;
	r = syscall_copy_user_string(frame->rdx, fstype, sizeof(fstype));
	if (r != VFS_OK)
		return r;

	return fs_mount_spec(source, target, fstype, frame->r10,
						 (const char *)frame->r8);
}

static long sys_change_root_handler(interrupt_frame_t *frame)
{
	char source[256];
	char fstype[32];
	char init_path[256];

	int r = syscall_copy_user_string(frame->rdi, source, sizeof(source));
	if (r != VFS_OK)
		return r;

	r = syscall_copy_user_string(frame->rsi, fstype, sizeof(fstype));
	if (r != VFS_OK)
		return r;

	r = syscall_copy_user_string(frame->rdx, init_path, sizeof(init_path));
	if (r != VFS_OK)
		return r;

	r = vfs_mkdir("/newroot", 0755, &vfs_root_cred);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;

	r = fs_mount_spec(source, "/newroot", fstype, 0, NULL);
	if (r != VFS_OK)
		return r;

	r = vfs_change_root("/newroot", &vfs_root_cred);
	if (r != VFS_OK)
		return r;

	r = fs_mount_spec("devfs", "/dev", "devfs", 0, NULL);
	if (r != VFS_OK) {
		log_err("syscall",
				"failed to mount devfs on new root /dev status=%s(%d)",
				vfs_err_name(r), r);
		return r;
	}

	log_debug("syscall", "mounted devfs on new root /dev");

	r = init_spawn(init_path);
	if (r != VFS_OK)
		return r;

	return (long)(uintptr_t)sched_syscall_exit(frame, 0);
}

static long sys_execve_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process || !thread->process->vas)
		return VFS_ERR_BADF;

	char path[EXEC_STR_MAX];
	int r = syscall_copy_user_string(frame->rdi, path, sizeof(path));
	if (r != VFS_OK)
		return r;

	uint64_t argv_ptrs[EXEC_ARG_MAX + 1];
	uint64_t env_ptrs[EXEC_ARG_MAX + 1];
	char arg_bufs[EXEC_ARG_MAX][EXEC_STR_MAX];
	char env_bufs[EXEC_ARG_MAX][EXEC_STR_MAX];
	size_t argc = 0;
	size_t envc = 0;
	r = syscall_copy_user_ptrs(frame->rsi, argv_ptrs, EXEC_ARG_MAX + 1, &argc);
	if (r != VFS_OK)
		return r;
	if (argc > EXEC_ARG_MAX)
		return VFS_ERR_NAMETOOLONG;

	r = syscall_copy_user_ptrs(frame->rdx, env_ptrs, EXEC_ARG_MAX + 1, &envc);
	if (r != VFS_OK)
		return r;
	if (envc > EXEC_ARG_MAX)
		return VFS_ERR_NAMETOOLONG;

	char *argv[EXEC_ARG_MAX];
	char *envp[EXEC_ARG_MAX];
	for (size_t i = 0; i < argc; i++) {
		r = syscall_copy_user_string(argv_ptrs[i], arg_bufs[i],
									 sizeof(arg_bufs[i]));
		if (r != VFS_OK)
			return r;
		argv[i] = arg_bufs[i];
	}
	for (size_t i = 0; i < envc; i++) {
		r = syscall_copy_user_string(env_ptrs[i], env_bufs[i],
									 sizeof(env_bufs[i]));
		if (r != VFS_OK)
			return r;
		envp[i] = env_bufs[i];
	}

	vas_t *new_vas = vas_create(NULL);
	if (!new_vas)
		return VFS_ERR_NOMEM;

	elf_user_image_t image;
	r = elf_load_user_executable(new_vas, path, &image);
	if (r != VFS_OK) {
		vas_destroy(new_vas);
		return r;
	}

	uint64_t stack_top = 0x00007ffffff000ULL;
	uint64_t stack_size = 16 * PAGE_SIZE;
	uint64_t stack_base = stack_top - stack_size;
	if (vas_map_anon(new_vas, stack_base, stack_size,
					 VMM_PRESENT | VMM_WRITABLE | VMM_USER | VMM_NX |
						 VAD_FIXED) != stack_base) {
		vas_destroy(new_vas);
		return VFS_ERR_NOMEM;
	}

	uint64_t user_rsp = 0;
	r = elf_build_initial_stack(
		new_vas, stack_top, path, (const char *const *)argv, argc,
		(const char *const *)envp, envc, &image, &user_rsp);
	if (r != VFS_OK) {
		vas_destroy(new_vas);
		return r;
	}

	log_debug(
		"execve",
		"path=%s entry=0x%llx prog_entry=0x%llx phdr=0x%llx phnum=%u rsp=0x%llx",
		path, image.entry, image.program_entry, image.program_phdr,
		image.program_phnum, user_rsp);
	vas_t *old_vas = thread->process->vas;
	thread->process->vas = new_vas;
	thread->process->pml4 = new_vas->pml4;
	thread->fs_base = 0;
	thread->user_entry_rsp = user_rsp;
	write_fs_base(0);
	vas_switch(new_vas);
	if (old_vas)
		vas_destroy(old_vas);

	memset(frame, 0, sizeof(*frame));
	frame->rip = image.entry;
	frame->cs = USER_CS;
	frame->rflags = 0x202;
	frame->rsp = user_rsp;
	frame->ss = 0x23;
	frame->ds = 0x23;
	frame->es = 0x23;
	thread->rsp = (uint64_t)frame;
	thread->mode = TCB_MODE_USER;
	return VFS_OK;
}

static long sys_arch_prctl_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread)
		return VFS_ERR_INVAL;

	switch ((long)frame->rdi) {
	case ARCH_SET_FS:
		if (frame->rsi >= VAS_USER_END)
			return VFS_ERR_INVAL;
		thread->fs_base = frame->rsi;
		write_fs_base(frame->rsi);
		return VFS_OK;
	case ARCH_GET_FS:
		if (!syscall_user_range_ok(frame->rsi, sizeof(uint64_t)))
			return VFS_ERR_INVAL;
		*(uint64_t *)frame->rsi = thread->fs_base;
		return VFS_OK;
	default:
		return VFS_ERR_NOSYS;
	}
}

static long sys_mmap_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process || !thread->process->vas)
		return VFS_ERR_BADF;

	uint64_t addr = frame->rdi;
	size_t length = (size_t)frame->rsi;
	int prot = (int)frame->rdx;
	int flags = (int)frame->r10;
	int fd = (int)frame->r8;
	uint64_t offset = frame->r9;
	(void)fd;
	(void)offset;

	if (!length || (flags & 0x01) || !(flags & 0x02) || !(flags & 0x20))
		return VFS_ERR_NOSYS;

	uint64_t vmm_flags = syscall_mmap_prot_to_vmm(prot);
	if (flags & 0x10)
		vmm_flags |= VAD_FIXED;

	uint64_t mapped =
		vas_map_anon(thread->process->vas, addr, length, vmm_flags);
	if (!mapped)
		return VFS_ERR_NOMEM;
	return (long)mapped;
}

static long sys_munmap_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process || !thread->process->vas)
		return VFS_ERR_BADF;
	return vas_unmap(thread->process->vas, frame->rdi, (size_t)frame->rsi);
}

static long sys_mprotect_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process || !thread->process->vas)
		return VFS_ERR_BADF;
	return vas_protect(thread->process->vas, frame->rdi, (size_t)frame->rsi,
					   syscall_mmap_prot_to_vmm((int)frame->rdx));
}

static long sys_mkdir_handler(interrupt_frame_t *frame)
{
	char path[256];
	int r = syscall_copy_user_string(frame->rdi, path, sizeof(path));
	if (r != VFS_OK)
		return r;
	return vfs_mkdir(path, (vfs_mode_t)frame->rsi, &vfs_root_cred);
}

static long sys_rmdir_handler(interrupt_frame_t *frame)
{
	char path[256];
	int r = syscall_copy_user_string(frame->rdi, path, sizeof(path));
	if (r != VFS_OK)
		return r;
	return vfs_rmdir(path, &vfs_root_cred);
}

static long sys_unlink_handler(interrupt_frame_t *frame)
{
	char path[256];
	int r = syscall_copy_user_string(frame->rdi, path, sizeof(path));
	if (r != VFS_OK)
		return r;
	return vfs_unlink(path, &vfs_root_cred);
}

static long sys_chmod_handler(interrupt_frame_t *frame)
{
	char path[256];
	int r = syscall_copy_user_string(frame->rdi, path, sizeof(path));
	if (r != VFS_OK)
		return r;
	return vfs_chmod(path, (vfs_mode_t)frame->rsi, &vfs_root_cred);
}

static long sys_chown_handler(interrupt_frame_t *frame)
{
	char path[256];
	int r = syscall_copy_user_string(frame->rdi, path, sizeof(path));
	if (r != VFS_OK)
		return r;
	return vfs_chown(path, (vfs_uid_t)frame->rsi, (vfs_gid_t)frame->rdx,
					 &vfs_root_cred);
}

static long sys_exit_handler(interrupt_frame_t *frame)
{
	return (long)(uintptr_t)sched_syscall_exit(frame, (int)frame->rdi);
}

static long sys_ioctl_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	int fd = (int)frame->rdi;
	unsigned long request = (unsigned long)frame->rsi;
	void *arg = (void *)frame->rdx;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return VFS_ERR_BADF;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	return vfs_ioctl(file, request, arg);
}

static syscall_handler_t syscall_table[] = {
	[SYS_READ] = sys_read_handler,
	[SYS_WRITE] = sys_write_handler,
	[SYS_OPEN] = sys_open_handler,
	[SYS_CLOSE] = sys_close_handler,
	[SYS_STAT] = sys_stat_handler,
	[SYS_LSEEK] = sys_lseek_handler,
	[SYS_ACCESS] = sys_access_handler,
	[SYS_GETDENTS] = sys_getdents_handler,
	[SYS_EXIT] = sys_exit_handler,
	[SYS_CHMOD] = sys_chmod_handler,
	[SYS_CHOWN] = sys_chown_handler,
	[SYS_MKDIR] = sys_mkdir_handler,
	[SYS_RMDIR] = sys_rmdir_handler,
	[SYS_UNLINK] = sys_unlink_handler,
	[SYS_CHROOT] = sys_chroot_handler,
	[SYS_MOUNT] = sys_mount_handler,
	[SYS_CHANGE_ROOT] = sys_change_root_handler,
	[SYS_EXECVE] = sys_execve_handler,
	[SYS_ARCH_PRCTL] = sys_arch_prctl_handler,
	[SYS_MMAP] = sys_mmap_handler,
	[SYS_MUNMAP] = sys_munmap_handler,
	[SYS_MPROTECT] = sys_mprotect_handler,
	[SYS_IOCTL] = sys_ioctl_handler,
};

interrupt_frame_t *syscall_dispatch(interrupt_frame_t *frame)
{
	if (!frame)
		return frame;

	uint64_t nr = frame->rax;
	if (nr < (sizeof(syscall_table) / sizeof(syscall_table[0])) &&
		syscall_table[nr]) {
		long ret = syscall_table[nr](frame);
		if (nr == SYS_EXIT || nr == SYS_CHANGE_ROOT)
			return (interrupt_frame_t *)(uintptr_t)ret;
		frame->rax = (uint64_t)(int64_t)ret;
		return frame;
	}

	tcb_t *thread = sched_current();
	log_warn("syscall", "unhandled syscall rax=%llu from tid=%d", nr,
			 thread ? thread->tid : -1);
	return frame;
}
