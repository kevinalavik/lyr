#include <sys/syscall.h>
#include <cpu/instr.h>
#include <errno.h>
#include <debug/log.h>
#include <fs/mount.h>
#include <fs/vfs.h>
#include <init/init.h>
#include <lib/elf.h>
#include <lib/string.h>
#include <mm/vmm.h>
#include <sched/sched.h>
#include <util/kprintf.h>
#include <mm/heap.h>
#include <net/net.h>
#include <net/socket.h>
#include <dev/time.h>
#include <dev/pit.h>
#include <sys/poll.h>

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


typedef struct {
	int64_t tv_sec;
	long tv_nsec;
} syscall_timespec_t;

static void syscall_relax_until_interrupt(void)
{
	__asm__ volatile("sti; hlt; cli" ::: "memory");
}

static vfs_file_t *syscall_dup_file(vfs_file_t *file)
{
	if (!file)
		return NULL;

	vfs_file_t *dup = kzalloc(sizeof(*dup));
	if (!dup)
		return NULL;

	*dup = *file;
	if (dup->node)
		vfs_node_ref(dup->node);
	if (dup->private_data) {
		if (net_socket_ref((socket_t *)dup->private_data) != NET_SOCK_OK) {
			if (dup->node)
				vfs_node_release(dup->node);
			kfree(dup);
			return NULL;
		}
	}
	return dup;
}

static void syscall_close_file_slot(vfs_file_t **slot)
{
	if (!slot || !*slot)
		return;
	vfs_file_t *file = *slot;
	if (file->private_data)
		net_close((socket_t *)file->private_data);
	vfs_close(file);
	*slot = NULL;
}


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

	if (file->private_data) {
		socket_t *sock = file->private_data;
		return net_recv(sock, buf, len, 0);
	}

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

	log_debug("syscall", "write: fd=%d buf=0x%llx len=%zu", fd, (uint64_t)buf,
			  len);

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return VFS_ERR_BADF;

	if (len && (!buf || !syscall_user_range_ok((uint64_t)buf, len)))
		return VFS_ERR_INVAL;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	if (file->private_data) {
		socket_t *sock = file->private_data;
		return net_send(sock, buf, len, 0);
	}

	if (!buf)
		return VFS_ERR_INVAL;

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

	vfs_file_t *file = thread->process->files[fd];
	if (file->private_data) {
		socket_t *sock = file->private_data;
		net_close(sock);
	}
	vfs_close(file);
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

	process_setup_fds(thread->process);

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

static long sys_socket_handler(interrupt_frame_t *frame)
{
	int domain = (int)frame->rdi;
	int type = (int)frame->rsi;
	int protocol = (int)frame->rdx;

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	socket_t *sock = NULL;
	int r = net_socket(&sock, domain, type, protocol);
	if (r != NET_SOCK_OK)
		return r;

	for (int fd = 0; fd < SCHED_FILE_MAX; fd++) {
		if (!thread->process->files[fd]) {
			vfs_file_t *file = kzalloc(sizeof(*file));
			if (!file) {
				net_close(sock);
				return VFS_ERR_NOMEM;
			}
			file->node = NULL;
			file->flags = 0;
			file->offset = 0;
			file->private_data = sock;
			thread->process->files[fd] = file;
			log_debug("syscall", "socket created fd=%d", fd);
			return fd;
		}
	}

	net_close(sock);
	return VFS_ERR_NOMEM;
}

static long sys_bind_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	uint64_t addr_ptr = frame->rsi;
	socklen_t addrlen = (socklen_t)frame->rdx;

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return VFS_ERR_BADF;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	socket_t *sock = file->private_data;
	if (!sock)
		return VFS_ERR_BADF;

	if (!addr_ptr || !syscall_user_range_ok(addr_ptr, addrlen))
		return VFS_ERR_INVAL;

	sockaddr_t addr;
	if (addrlen > sizeof(addr))
		addrlen = sizeof(addr);
	memcpy(&addr, (void *)addr_ptr, addrlen);

	return net_bind(sock, &addr, addrlen);
}

static long sys_connect_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	uint64_t addr_ptr = frame->rsi;
	socklen_t addrlen = (socklen_t)frame->rdx;

	log_debug("syscall", "connect: fd=%d addr=%llx len=%d", fd, addr_ptr,
			  addrlen);

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return VFS_ERR_BADF;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	socket_t *sock = file->private_data;
	if (!sock)
		return VFS_ERR_BADF;

	if (!addr_ptr || !syscall_user_range_ok(addr_ptr, addrlen))
		return VFS_ERR_INVAL;

	sockaddr_t addr;
	if (addrlen > sizeof(addr))
		addrlen = sizeof(addr);
	memcpy(&addr, (void *)addr_ptr, addrlen);

	log_debug("syscall", "connect: calling net_connect");
	long r = net_connect(sock, &addr, addrlen);
	log_debug("syscall", "connect: net_connect returned %ld", r);
	return r;
}

static long sys_listen_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	int backlog = (int)frame->rsi;

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return VFS_ERR_BADF;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	socket_t *sock = file->private_data;
	if (!sock)
		return VFS_ERR_BADF;

	return net_listen(sock, backlog);
}

static long sys_accept_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	uint64_t addr_ptr = frame->rsi;
	uint64_t addrlen_ptr = frame->rdx;

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return VFS_ERR_BADF;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	socket_t *sock = file->private_data;
	if (!sock)
		return VFS_ERR_BADF;

	socket_t *client = NULL;
	sockaddr_t addr;
	socklen_t addrlen = 0;

	if (addrlen_ptr && syscall_user_range_ok(addrlen_ptr, sizeof(socklen_t))) {
		addrlen = *(socklen_t *)addrlen_ptr;
	}

	int r = net_accept(sock, &client, &addr, &addrlen);
	if (r != NET_SOCK_OK)
		return r;

	for (int newfd = 0; newfd < SCHED_FILE_MAX; newfd++) {
		if (!thread->process->files[newfd]) {
			vfs_file_t *newfile = kzalloc(sizeof(*newfile));
			if (!newfile) {
				net_close(client);
				return VFS_ERR_NOMEM;
			}
			newfile->node = NULL;
			newfile->flags = 0;
			newfile->offset = 0;
			newfile->private_data = client;
			thread->process->files[newfd] = newfile;

			if (addr_ptr && addrlen_ptr &&
				syscall_user_range_ok(addr_ptr, sizeof(sockaddr_t)) &&
				syscall_user_range_ok(addrlen_ptr, sizeof(socklen_t))) {
				memcpy((void *)addr_ptr, &addr,
					   addrlen < sizeof(sockaddr_t) ? addrlen :
													  sizeof(sockaddr_t));
				*(socklen_t *)addrlen_ptr = addrlen;
			}

			log_debug("syscall", "accept created fd=%d", newfd);
			return newfd;
		}
	}

	net_close(client);
	return VFS_ERR_NOMEM;
}

static long sys_send_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	uint64_t buf_ptr = frame->rsi;
	size_t len = (size_t)frame->rdx;
	int flags = (int)frame->r10;

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX || !buf_ptr ||
		!syscall_user_range_ok(buf_ptr, len))
		return VFS_ERR_INVAL;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	socket_t *sock = file->private_data;
	if (!sock)
		return VFS_ERR_BADF;

	return net_send(sock, (const void *)buf_ptr, len, flags);
}

static long sys_recv_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	uint64_t buf_ptr = frame->rsi;
	size_t len = (size_t)frame->rdx;
	int flags = (int)frame->r10;

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX || !buf_ptr ||
		!syscall_user_range_ok(buf_ptr, len))
		return VFS_ERR_INVAL;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	socket_t *sock = file->private_data;
	if (!sock)
		return VFS_ERR_BADF;

	return net_recv(sock, (void *)buf_ptr, len, flags);
}

static long sys_sendto_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	uint64_t buf_ptr = frame->rsi;
	size_t len = (size_t)frame->rdx;
	int flags = (int)frame->r10;
	uint64_t dest_ptr = frame->r8;
	socklen_t dest_len = (socklen_t)frame->r9;

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX || !buf_ptr ||
		!syscall_user_range_ok(buf_ptr, len))
		return VFS_ERR_INVAL;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	socket_t *sock = file->private_data;
	if (!sock)
		return VFS_ERR_BADF;

	sockaddr_t dest;
	if (dest_ptr && syscall_user_range_ok(dest_ptr, dest_len)) {
		if (dest_len > sizeof(dest))
			dest_len = sizeof(dest);
		memcpy(&dest, (void *)dest_ptr, dest_len);
	} else {
		memset(&dest, 0, sizeof(dest));
		dest_len = 0;
	}

	return net_sendto(sock, (const void *)buf_ptr, len, flags, &dest, dest_len);
}

static long sys_recvfrom_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	uint64_t buf_ptr = frame->rsi;
	size_t len = (size_t)frame->rdx;
	int flags = (int)frame->r10;
	uint64_t addr_ptr = frame->r8;
	uint64_t addrlen_ptr = frame->r9;

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX || !buf_ptr ||
		!syscall_user_range_ok(buf_ptr, len))
		return VFS_ERR_INVAL;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	socket_t *sock = file->private_data;
	if (!sock)
		return VFS_ERR_BADF;

	sockaddr_t addr;
	socklen_t addrlen = 0;

	if (addr_ptr && addrlen_ptr &&
		syscall_user_range_ok(addrlen_ptr, sizeof(socklen_t))) {
		addrlen = *(socklen_t *)addrlen_ptr;
	}

	int r = net_recvfrom(sock, (void *)buf_ptr, len, flags, &addr, &addrlen);

	if (r >= 0 && addr_ptr && addrlen_ptr &&
		syscall_user_range_ok(addr_ptr, sizeof(sockaddr_t)) &&
		syscall_user_range_ok(addrlen_ptr, sizeof(socklen_t))) {
		memcpy((void *)addr_ptr, &addr,
			   addrlen < sizeof(sockaddr_t) ? addrlen : sizeof(sockaddr_t));
		*(socklen_t *)addrlen_ptr = addrlen;
	}

	return r;
}

static long sys_shutdown_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	int how = (int)frame->rsi;

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return VFS_ERR_BADF;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	socket_t *sock = file->private_data;
	if (!sock)
		return VFS_ERR_BADF;

	return net_shutdown(sock, how);
}

static long sys_getsockname_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	uint64_t addr_ptr = frame->rsi;
	uint64_t addrlen_ptr = frame->rdx;

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return VFS_ERR_BADF;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	socket_t *sock = file->private_data;
	if (!sock)
		return VFS_ERR_BADF;

	socklen_t addrlen = 0;
	if (addrlen_ptr && syscall_user_range_ok(addrlen_ptr, sizeof(socklen_t))) {
		addrlen = *(socklen_t *)addrlen_ptr;
	}

	sockaddr_t addr;
	int r = net_getsockname(sock, &addr, &addrlen);
	if (r != NET_SOCK_OK)
		return r;

	if (addr_ptr && addrlen_ptr &&
		syscall_user_range_ok(addr_ptr, sizeof(sockaddr_t)) &&
		syscall_user_range_ok(addrlen_ptr, sizeof(socklen_t))) {
		memcpy((void *)addr_ptr, &addr,
			   addrlen < sizeof(sockaddr_t) ? addrlen : sizeof(sockaddr_t));
		*(socklen_t *)addrlen_ptr = addrlen;
	}

	return NET_SOCK_OK;
}

static long sys_getpeername_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	uint64_t addr_ptr = frame->rsi;
	uint64_t addrlen_ptr = frame->rdx;

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return VFS_ERR_BADF;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	socket_t *sock = file->private_data;
	if (!sock)
		return VFS_ERR_BADF;

	socklen_t addrlen = 0;
	if (addrlen_ptr && syscall_user_range_ok(addrlen_ptr, sizeof(socklen_t))) {
		addrlen = *(socklen_t *)addrlen_ptr;
	}

	sockaddr_t addr;
	int r = net_getpeername(sock, &addr, &addrlen);
	if (r != NET_SOCK_OK)
		return r;

	if (addr_ptr && addrlen_ptr &&
		syscall_user_range_ok(addr_ptr, sizeof(sockaddr_t)) &&
		syscall_user_range_ok(addrlen_ptr, sizeof(socklen_t))) {
		memcpy((void *)addr_ptr, &addr,
			   addrlen < sizeof(sockaddr_t) ? addrlen : sizeof(sockaddr_t));
		*(socklen_t *)addrlen_ptr = addrlen;
	}

	return NET_SOCK_OK;
}

static long sys_setsockopt_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	int level = (int)frame->rsi;
	int optname = (int)frame->rdx;
	uint64_t optval_ptr = frame->r10;
	socklen_t optlen = (socklen_t)frame->r8;

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return VFS_ERR_BADF;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	socket_t *sock = file->private_data;
	if (!sock)
		return VFS_ERR_BADF;

	void *optval = NULL;
	char optbuf[256];

	if (optval_ptr && optlen > 0 && optlen <= sizeof(optbuf) &&
		syscall_user_range_ok(optval_ptr, optlen)) {
		memcpy(optbuf, (void *)optval_ptr, optlen);
		optval = optbuf;
	}

	return net_setsockopt(sock, level, optname, optval, optlen);
}

static long sys_getsockopt_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	int level = (int)frame->rsi;
	int optname = (int)frame->rdx;
	uint64_t optval_ptr = frame->r10;
	uint64_t optlen_ptr = frame->r8;

	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return VFS_ERR_BADF;

	vfs_file_t *file = thread->process->files[fd];
	if (!file)
		return VFS_ERR_BADF;

	socket_t *sock = file->private_data;
	if (!sock)
		return VFS_ERR_BADF;

	socklen_t optlen = 0;
	if (optlen_ptr && syscall_user_range_ok(optlen_ptr, sizeof(socklen_t))) {
		optlen = *(socklen_t *)optlen_ptr;
	}

	char optbuf[256];
	socklen_t actual_len = sizeof(optbuf);
	int r = net_getsockopt(sock, level, optname, optbuf, &actual_len);
	if (r != NET_SOCK_OK)
		return r;

	if (optval_ptr && optlen_ptr &&
		syscall_user_range_ok(optval_ptr, actual_len) &&
		syscall_user_range_ok(optlen_ptr, sizeof(socklen_t))) {
		memcpy((void *)optval_ptr, optbuf, actual_len);
		*(socklen_t *)optlen_ptr = actual_len;
	}

	return NET_SOCK_OK;
}

#define SYS_POLL_NFDS_MAX 1024

static int sys_poll_scan(struct lyr_pollfd *fds, size_t nfds)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	int ready = 0;
	net_poll_all();

	for (size_t i = 0; i < nfds; i++) {
		fds[i].revents = 0;

		if (fds[i].fd < 0)
			continue;

		if (fds[i].fd >= SCHED_FILE_MAX) {
			fds[i].revents = LYR_POLLNVAL;
			ready++;
			continue;
		}

		vfs_file_t *file = thread->process->files[fds[i].fd];
		if (!file) {
			fds[i].revents = LYR_POLLNVAL;
			ready++;
			continue;
		}

		if (file->private_data) {
			fds[i].revents = (int16_t)net_poll_socket(
				(socket_t *)file->private_data, fds[i].events);
		} else {
			/* wacky fix */
			if (fds[i].events & LYR_POLL_READ_MASK)
				fds[i].revents |= LYR_POLLIN | LYR_POLLRDNORM;
			if (fds[i].events & LYR_POLL_WRITE_MASK)
				fds[i].revents |= LYR_POLLOUT | LYR_POLLWRNORM;
		}

		if (fds[i].revents)
			ready++;
	}

	return ready;
}

static uint64_t sys_poll_timeout_ticks(uint64_t timeout_ms)
{
	uint64_t hz = pit_get_hz();
	uint64_t ticks = (timeout_ms * hz + 999) / 1000;
	return ticks ? ticks : 1;
}

static long sys_clock_get_handler(interrupt_frame_t *frame)
{
	int clock_id = (int)frame->rdi;
	uint64_t user_ts = frame->rsi;
	if (!syscall_user_range_ok(user_ts, sizeof(syscall_timespec_t)))
		return VFS_ERR_INVAL;

	int64_t sec = 0;
	long nsec = 0;
	int r = time_get(clock_id, &sec, &nsec);
	if (r != 0)
		return r;

	syscall_timespec_t ts = { .tv_sec = sec, .tv_nsec = nsec };
	memcpy((void *)user_ts, &ts, sizeof(ts));
	return VFS_OK;
}

static long sys_poll_handler(interrupt_frame_t *frame)
{
	uint64_t user_fds = frame->rdi;
	size_t nfds = (size_t)frame->rsi;
	int timeout_ms = (int)frame->rdx;

	if (nfds > SYS_POLL_NFDS_MAX)
		return VFS_ERR_INVAL;
	if (nfds && !syscall_user_range_ok(user_fds, nfds * sizeof(struct lyr_pollfd)))
		return VFS_ERR_INVAL;

	struct lyr_pollfd *fds = NULL;
	if (nfds) {
		fds = kzalloc(nfds * sizeof(*fds));
		if (!fds)
			return VFS_ERR_NOMEM;
		memcpy(fds, (void *)user_fds, nfds * sizeof(*fds));
	}

	uint64_t deadline = 0;
	int finite_timeout = timeout_ms >= 0;
	if (finite_timeout)
		deadline = pit_get_ticks() + sys_poll_timeout_ticks((uint64_t)timeout_ms);

	long ret = 0;
	for (;;) {
		ret = nfds ? sys_poll_scan(fds, nfds) : 0;
		if (ret < 0 || ret > 0)
			break;
		if (finite_timeout && timeout_ms == 0)
			break;
		if (finite_timeout && pit_get_ticks() >= deadline)
			break;
		syscall_relax_until_interrupt();
	}

	if (nfds && ret >= 0)
		memcpy((void *)user_fds, fds, nfds * sizeof(*fds));
	if (fds)
		kfree(fds);
	return ret;
}

static long sys_getpid_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return VFS_ERR_BADF;

	return (long)thread->process->pid;
}

static long sys_fork_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process || !thread->process->vas)
		return VFS_ERR_BADF;

	vas_t *child_vas = vas_clone(thread->process->vas);
	if (!child_vas)
		return VFS_ERR_NOMEM;

	pcb_t *child = sched_process_create(thread->process->name, child_vas);
	if (!child) {
		vas_destroy(child_vas);
		return VFS_ERR_NOMEM;
	}

	for (int i = 0; i < SCHED_FILE_MAX; i++)
		syscall_close_file_slot(&child->files[i]);

	for (int i = 0; i < SCHED_FILE_MAX; i++) {
		if (!thread->process->files[i])
			continue;
		child->files[i] = syscall_dup_file(thread->process->files[i]);
		if (!child->files[i]) {
			for (int j = 0; j < SCHED_FILE_MAX; j++)
				syscall_close_file_slot(&child->files[j]);
			return VFS_ERR_NOMEM;
		}
	}

	tcb_t *child_thread = sched_fork_thread(child, thread->name, frame);
	if (!child_thread) {
		for (int i = 0; i < SCHED_FILE_MAX; i++)
			syscall_close_file_slot(&child->files[i]);
		return VFS_ERR_NOMEM;
	}
	child_thread->fs_base = thread->fs_base;

	return (long)child->pid;
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
	[SYS_SOCKET] = sys_socket_handler,
	[SYS_BIND] = sys_bind_handler,
	[SYS_CONNECT] = sys_connect_handler,
	[SYS_LISTEN] = sys_listen_handler,
	[SYS_ACCEPT] = sys_accept_handler,
	[SYS_GETSOCKNAME] = sys_getsockname_handler,
	[SYS_GETPEERNAME] = sys_getpeername_handler,
	[SYS_SEND] = sys_send_handler,
	[SYS_RECV] = sys_recv_handler,
	[SYS_SENDTO] = sys_sendto_handler,
	[SYS_RECVFROM] = sys_recvfrom_handler,
	[SYS_SHUTDOWN] = sys_shutdown_handler,
	[SYS_SETSOCKOPT] = sys_setsockopt_handler,
	[SYS_GETSOCKOPT] = sys_getsockopt_handler,
	[SYS_CLOCK_GET] = sys_clock_get_handler,
	[SYS_POLL] = sys_poll_handler,
	[SYS_FORK] = sys_fork_handler,
	[SYS_GETPID] = sys_getpid_handler,
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
	frame->rax = (uint64_t)(int64_t)-ENOSYS;
	return frame;
}
