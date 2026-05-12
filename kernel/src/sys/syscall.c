#include <sys/syscall.h>
#include <cpu/instr.h>
#include <errno.h>
#include <debug/log.h>
#include <fs/mount.h>
#include <fs/pipe.h>
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
#include <stdarg.h>
#include <debug/panic.h>

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

#define SYS_F_DUPFD 0
#define SYS_F_GETFD 1
#define SYS_F_SETFD 2
#define SYS_F_GETFL 3
#define SYS_F_SETFL 4
#define SYS_F_DUPFD_CLOEXEC 1030

#define SYS_FD_CLOEXEC 1
#define SYS_O_CLOEXEC 02000000u
#define SYS_O_NONBLOCK SOCK_NONBLOCK
#define SYS_AT_FDCWD (-100)

#define EXEC_ARG_MAX 32
#define EXEC_STR_MAX 256
#define SYS_PATH_MAX 512

#define SYS_POLL_NFDS_MAX 1024
#define SYS_USER_STACK_GUARD_SIZE PAGE_SIZE
#define SYS_USER_STACK_SIZE (16 * PAGE_SIZE)

#define SYS_PROT_READ 0x1
#define SYS_PROT_WRITE 0x2
#define SYS_PROT_EXEC 0x4

#define SYS_MAP_SHARED 0x01
#define SYS_MAP_PRIVATE 0x02
#define SYS_MAP_FIXED 0x10
#define SYS_MAP_ANONYMOUS 0x20

#define SYS_SIGTERM 15
#define SYS_SIGPIPE 13
#define SYS_SIGKILL 9

#define SYS_NSEC_PER_SEC 1000000000LL
#define SYS_NSEC_PER_MSEC 1000000LL
#define SYS_USEC_PER_SEC 1000000LL
#define SYS_MSEC_PER_SEC 1000ULL

extern void syscall_entry(void);

typedef long (*syscall_handler_t)(interrupt_frame_t *frame);

#define SYS_DT_UNKNOWN 0
#define SYS_DT_FIFO 1
#define SYS_DT_CHR 2
#define SYS_DT_DIR 4
#define SYS_DT_BLK 6
#define SYS_DT_REG 8
#define SYS_DT_LNK 10
#define SYS_DT_SOCK 12

typedef struct {
	uint64_t d_ino;
	int64_t d_off;
	uint16_t d_reclen;
	uint8_t d_type;
	char d_name[];
} syscall_dirent_t;

typedef struct {
	int64_t tv_sec;
	long tv_nsec;
} syscall_timespec_t;

typedef struct {
	int64_t tv_sec;
	long tv_usec;
} syscall_timeval_t;

typedef struct {
	uint64_t st_dev;
	uint64_t st_ino;
	uint64_t st_nlink;
	vfs_mode_t st_mode;
	vfs_uid_t st_uid;
	vfs_gid_t st_gid;
	uint32_t __pad0;
	uint64_t st_rdev;
	int64_t st_size;
	int64_t st_blksize;
	int64_t st_blocks;
	syscall_timespec_t st_atim;
	syscall_timespec_t st_mtim;
	syscall_timespec_t st_ctim;
	int64_t __unused[3];
} syscall_stat_t;

#define SYS_UTSNAME_FIELD_LEN 65

typedef struct {
	char sysname[SYS_UTSNAME_FIELD_LEN];
	char nodename[SYS_UTSNAME_FIELD_LEN];
	char release[SYS_UTSNAME_FIELD_LEN];
	char version[SYS_UTSNAME_FIELD_LEN];
	char machine[SYS_UTSNAME_FIELD_LEN];
	char domainname[SYS_UTSNAME_FIELD_LEN];
} syscall_utsname_t;

typedef struct {
	uint64_t handler;
	uint64_t flags;
	uint64_t restorer;
	uint64_t mask;
} syscall_sigaction_t;

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

static pcb_t *syscall_current_process(void)
{
	tcb_t *thread = sched_current();
	return thread ? thread->process : NULL;
}

static const vfs_cred_t *syscall_current_cred(void)
{
	return sched_process_cred(syscall_current_process());
}

static int syscall_user_range_ok(uint64_t addr, size_t len)
{
	if (addr >= VAS_USER_END)
		return 0;

	if (len > VAS_USER_END - addr)
		return 0;

	pcb_t *process = syscall_current_process();
	if (!process || !process->vas)
		return 0;

	return vas_user_access_ok(process->vas, addr, len, 0) == 0;
}

static int syscall_user_range_write_ok(uint64_t addr, size_t len)
{
	if (addr >= VAS_USER_END)
		return 0;

	if (len > VAS_USER_END - addr)
		return 0;

	pcb_t *process = syscall_current_process();
	if (!process || !process->vas)
		return 0;

	return vas_user_access_ok(process->vas, addr, len, 1) == 0;
}

static int syscall_copy_from_user(void *dst, uint64_t user, size_t len)
{
	if (len == 0)
		return 0;
	if (!dst || !syscall_user_range_ok(user, len))
		return -EFAULT;
	memcpy(dst, (const void *)(uintptr_t)user, len);
	return 0;
}

static int syscall_copy_to_user(uint64_t user, const void *src, size_t len)
{
	if (len == 0)
		return 0;
	if (!src || !syscall_user_range_write_ok(user, len))
		return -EFAULT;
	memcpy((void *)(uintptr_t)user, src, len);
	return 0;
}

static int syscall_copy_user_string(uint64_t user, char *out, size_t out_len)
{
	if (!user || user >= VAS_USER_END || !out || out_len == 0)
		return -EINVAL;

	for (size_t i = 0; i < out_len; i++) {
		char c;
		if (syscall_copy_from_user(&c, user + i, sizeof(c)) != 0)
			return -EFAULT;
		out[i] = c;

		if (c == '\0')
			return 0;
	}

	out[out_len - 1] = '\0';
	return -ENAMETOOLONG;
}

static int syscall_interrupted(void)
{
	tcb_t *thread = sched_current();
	return (thread && sched_signal_is_pending(thread)) ? -EINTR : 0;
}

static int syscall_normalize_path(const char *input, char *out, size_t out_len)
{
	char tmp[SYS_PATH_MAX];
	size_t pos = 0;

	if (!input || !out || out_len < 2 || input[0] == '\0')
		return -EINVAL;

	if (input[0] == '/') {
		size_t len = strlen(input);

		if (len >= sizeof(tmp))
			return -ENAMETOOLONG;

		memcpy(tmp, input, len + 1);
	} else {
		pcb_t *process = syscall_current_process();
		const char *cwd = sched_process_cwd(process);
		size_t cwd_len = strlen(cwd);
		size_t input_len = strlen(input);
		int need_slash = !(cwd_len == 1 && cwd[0] == '/');

		if (cwd_len + (size_t)need_slash + input_len >= sizeof(tmp))
			return -ENAMETOOLONG;

		memcpy(tmp, cwd, cwd_len);
		pos = cwd_len;

		if (need_slash)
			tmp[pos++] = '/';

		memcpy(tmp + pos, input, input_len + 1);
	}

	out[0] = '/';
	out[1] = '\0';
	pos = 1;

	const char *p = tmp;

	while (*p) {
		while (*p == '/')
			p++;

		if (!*p)
			break;

		const char *start = p;

		while (*p && *p != '/')
			p++;

		size_t len = (size_t)(p - start);

		if (len == 1 && start[0] == '.')
			continue;

		if (len == 2 && start[0] == '.' && start[1] == '.') {
			if (pos > 1) {
				if (out[pos - 1] == '/')
					pos--;

				while (pos > 1 && out[pos - 1] != '/')
					pos--;

				out[pos] = '\0';
			}

			continue;
		}

		if (pos > 1) {
			if (pos + 1 >= out_len)
				return -ENAMETOOLONG;

			out[pos++] = '/';
		}

		if (pos + len >= out_len)
			return -ENAMETOOLONG;

		memcpy(out + pos, start, len);
		pos += len;
		out[pos] = '\0';
	}

	return 0;
}

static int syscall_copy_user_path_abs(uint64_t user, char *out, size_t out_len)
{
	char raw[SYS_PATH_MAX];
	int r = syscall_copy_user_string(user, raw, sizeof(raw));

	if (r != 0)
		return r;

	return syscall_normalize_path(raw, out, out_len);
}

static int syscall_copy_user_ptrs(uint64_t user, uint64_t *out, size_t cap,
								  size_t *count_out)
{
	if (!out || !count_out)
		return -EINVAL;

	*count_out = 0;

	if (!user)
		return 0;

	for (size_t i = 0; i < cap; i++) {
		if (!syscall_user_range_ok(user + i * sizeof(uint64_t),
								   sizeof(uint64_t)))
			return -EINVAL;

		uint64_t ptr = ((const uint64_t *)user)[i];

		if (!ptr)
			return 0;

		out[i] = ptr;
		*count_out = i + 1;
	}

	return -ENAMETOOLONG;
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

static int syscall_mmap_fill_private(vfs_file_t *file, uint64_t addr,
									 size_t length, uint64_t offset)
{
	vfs_node_t *node = vfs_file_node(file);
	size_t copied = 0;

	if (!node || !node->ops || !node->ops->read)
		return -ENOSYS;

	while (copied < length) {
		size_t done = 0;
		int r = node->ops->read(node, offset + copied, (void *)(addr + copied),
								length - copied, &done);
		if (r != 0)
			return r;
		if (done == 0)
			break;
		copied += done;
	}

	return 0;
}

static long syscall_copy_stat_out(uint64_t user, const vfs_stat_t *st)
{
	if (!st || !syscall_user_range_ok(user, sizeof(syscall_stat_t)))
		return -EINVAL;

	syscall_stat_t out;
	memset(&out, 0, sizeof(out));

	out.st_dev = st->dev;
	out.st_ino = st->ino;
	out.st_nlink = st->nlink ? st->nlink : 1;
	out.st_mode = st->mode;
	out.st_uid = st->uid;
	out.st_gid = st->gid;
	out.st_rdev = 0;
	out.st_size = (int64_t)st->size;
	out.st_blksize = st->blksize ? (int64_t)st->blksize : 4096;
	out.st_blocks = (int64_t)st->blocks;

	memcpy((void *)user, &out, sizeof(out));
	return 0;
}

static size_t syscall_strnlen(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n])
		n++;

	return n;
}

static size_t syscall_align_up(size_t value, size_t align)
{
	return (value + align - 1) & ~(align - 1);
}

static void syscall_uts_copy(char dst[SYS_UTSNAME_FIELD_LEN], const char *src)
{
	size_t i = 0;

	if (!src)
		src = "";

	while (i + 1 < SYS_UTSNAME_FIELD_LEN && src[i]) {
		dst[i] = src[i];
		i++;
	}

	dst[i] = '\0';
}

static void syscall_uts_copyf(char dst[SYS_UTSNAME_FIELD_LEN], const char *fmt,
							  ...)
{
	va_list args;

	if (!fmt) {
		dst[0] = '\0';
		return;
	}

	va_start(args, fmt);
	vsnprintf(dst, SYS_UTSNAME_FIELD_LEN, fmt, args);
	va_end(args);

	dst[SYS_UTSNAME_FIELD_LEN - 1] = '\0';
}

static uint8_t syscall_dirent_type(vfs_mode_t mode)
{
	if (VFS_S_ISDIR(mode))
		return SYS_DT_DIR;

	return SYS_DT_REG;
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

static int syscall_alloc_fd(pcb_t *process, int minfd)
{
	if (!process || minfd < 0)
		return -EINVAL;

	for (int fd = minfd; fd < SCHED_FILE_MAX; fd++) {
		if (!process->files[fd])
			return fd;
	}

	return -ENOMEM;
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

static int syscall_copy_timespec_timeout(uint64_t user_ts,
										 time_timeout_t *timeout)
{
	if (!timeout)
		return -EINVAL;

	if (!user_ts)
		return time_timeout_after_ms(-1, timeout);

	syscall_timespec_t ts;
	int r = syscall_copy_from_user(&ts, user_ts, sizeof(ts));
	if (r != 0)
		return r;

	r = time_timeout_after_timespec(ts.tv_sec, ts.tv_nsec, timeout);

	return r == 0 ? 0 : r;
}

/* -------------------------------------------------------------------------- */
/* Basic file syscalls                                                        */
/* -------------------------------------------------------------------------- */

static long sys_read_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	int fd = (int)frame->rdi;
	uint64_t user_buf = frame->rsi;
	size_t len = (size_t)frame->rdx;

	if (fd < 0 || fd >= SCHED_FILE_MAX || !user_buf)
		return -EINVAL;
	if (len && !syscall_user_range_write_ok(user_buf, len))
		return -EFAULT;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	if (len == 0)
		return 0;

	size_t total = 0;
	size_t chunk_cap = len < PAGE_SIZE ? len : PAGE_SIZE;
	char *kbuf = kmalloc(chunk_cap);
	if (!kbuf)
		return -ENOMEM;

	while (total < len) {
		size_t chunk = len - total;
		if (chunk > chunk_cap)
			chunk = chunk_cap;

		size_t done = 0;
		int r;
		if (file->private_data)
			r = net_recv((socket_t *)file->private_data, kbuf, chunk, 0);
		else if (vfs_pipe_is(file))
			r = vfs_pipe_read(file, kbuf, chunk, &done);
		else
			r = vfs_read(file, kbuf, chunk, &done);

		if (file->private_data) {
			if (r < 0) {
				kfree(kbuf);
				return total ? (long)total : r;
			}
			done = (size_t)r;
			r = 0;
		}

		if (r != 0) {
			kfree(kbuf);
			return total ? (long)total : r;
		}
		if (done == 0)
			break;
		if (syscall_copy_to_user(user_buf + total, kbuf, done) != 0) {
			kfree(kbuf);
			return total ? (long)total : -EFAULT;
		}

		total += done;
		if (done < chunk)
			break;
	}

	kfree(kbuf);
	return (long)total;
}

static long sys_write_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	int fd = (int)frame->rdi;
	uint64_t user_buf = frame->rsi;
	size_t len = (size_t)frame->rdx;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return -EBADF;

	if (len && (!user_buf || !syscall_user_range_ok(user_buf, len)))
		return -EFAULT;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	if (!user_buf)
		return -EINVAL;
	if (len == 0)
		return 0;

	size_t total = 0;
	size_t chunk_cap = len < PAGE_SIZE ? len : PAGE_SIZE;
	char *kbuf = kmalloc(chunk_cap);
	if (!kbuf)
		return -ENOMEM;

	while (total < len) {
		size_t chunk = len - total;
		if (chunk > chunk_cap)
			chunk = chunk_cap;
		if (syscall_copy_from_user(kbuf, user_buf + total, chunk) != 0) {
			kfree(kbuf);
			return total ? (long)total : -EFAULT;
		}

		size_t done = 0;
		int r;
		if (file->private_data) {
			r = net_send((socket_t *)file->private_data, kbuf, chunk, 0);
			if (r < 0) {
				kfree(kbuf);
				return total ? (long)total : r;
			}
			done = (size_t)r;
			r = 0;
		} else if (vfs_pipe_is(file)) {
			r = vfs_pipe_write(file, kbuf, chunk, &done);
			if (r == -EPIPE) {
				(void)sched_process_signal(thread->process,
										   thread->process->pid, SYS_SIGPIPE);
			}
		} else {
			r = vfs_write(file, kbuf, chunk, &done);
		}

		if (r != 0) {
			kfree(kbuf);
			return total ? (long)total : r;
		}
		if (done == 0)
			break;
		total += done;
		if (done < chunk)
			break;
	}

	kfree(kbuf);
	return (long)total;
}

static long sys_open_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	char path[SYS_PATH_MAX];
	int r = syscall_copy_user_path_abs(frame->rdi, path, sizeof(path));

	if (r != 0)
		return r;

	for (int fd = 0; fd < SCHED_FILE_MAX; fd++) {
		if (thread->process->files[fd])
			continue;

		vfs_file_t *file = NULL;
		r = vfs_open(path, (uint32_t)frame->rsi, (vfs_mode_t)frame->rdx,
					 syscall_current_cred(), &file);

		if (r != 0)
			return r;

		thread->process->files[fd] = file;
		return fd;
	}

	return -ENOMEM;
}

static long sys_close_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	int fd = (int)frame->rdi;

	if (fd < 0 || fd >= SCHED_FILE_MAX || !thread->process->files[fd])
		return -EBADF;

	vfs_file_t *file = thread->process->files[fd];

	if (file->private_data)
		net_close((socket_t *)file->private_data);

	vfs_close(file);
	thread->process->files[fd] = NULL;
	thread->process->fd_flags[fd] = 0;

	return 0;
}

static long sys_fcntl_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	int fd = (int)frame->rdi;
	int cmd = (int)frame->rsi;
	long arg = (long)frame->rdx;
	pcb_t *process = thread->process;

	if (fd < 0 || fd >= SCHED_FILE_MAX || !process->files[fd])
		return -EBADF;

	long ret = 0;
	long err = 0;

	switch (cmd) {
	case SYS_F_DUPFD:
	case SYS_F_DUPFD_CLOEXEC: {
		int minfd = (int)arg;

		if (minfd < 0) {
			err = -EINVAL;
			break;
		}

		if (minfd >= SCHED_FILE_MAX) {
			err = -EBADF;
			break;
		}

		err = -ENOMEM;

		for (int newfd = minfd; newfd < SCHED_FILE_MAX; newfd++) {
			if (process->files[newfd])
				continue;

			vfs_file_t *dup = syscall_dup_file(process->files[fd]);
			if (!dup) {
				err = -ENOMEM;
				break;
			}

			process->files[newfd] = dup;
			process->fd_flags[newfd] =
				(cmd == SYS_F_DUPFD_CLOEXEC) ? SYS_FD_CLOEXEC : 0;

			ret = newfd;
			err = 0;
			break;
		}

		break;
	}

	case SYS_F_GETFD:
		ret = (long)(process->fd_flags[fd] & SYS_FD_CLOEXEC);
		err = 0;
		break;

	case SYS_F_SETFD:
		process->fd_flags[fd] = (uint32_t)arg & SYS_FD_CLOEXEC;
		ret = 0;
		err = 0;
		break;

	case SYS_F_GETFL: {
		vfs_file_t *file = process->files[fd];
		uint32_t flags = file->flags;

		if (file->private_data) {
			uint32_t sock_flags = 0;
			int r = net_socket_get_status_flags((socket_t *)file->private_data,
												&sock_flags);

			if (r != NET_SOCK_OK) {
				err = r;
				break;
			}

			flags |= sock_flags;
		}

		ret = (long)flags;
		err = 0;
		break;
	}

	case SYS_F_SETFL: {
		vfs_file_t *file = process->files[fd];
		uint32_t flags = (uint32_t)arg;

		if (file->private_data) {
			int r = net_socket_set_status_flags((socket_t *)file->private_data,
												flags);

			if (r != NET_SOCK_OK) {
				err = r;
				break;
			}
		} else {
			if (flags & SYS_O_NONBLOCK)
				file->flags |= SYS_O_NONBLOCK;
			else
				file->flags &= ~SYS_O_NONBLOCK;
		}

		ret = 0;
		err = 0;
		break;
	}

	default:
		err = -EINVAL;
		break;
	}

	if (err != 0)
		return err;

	return ret;
}

static long sys_dup2_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return -EBADF;

	int oldfd = (int)frame->rdi;
	int newfd = (int)frame->rsi;
	int flags = (int)frame->rdx;
	pcb_t *process = thread->process;

	if (oldfd < 0 || oldfd >= SCHED_FILE_MAX || newfd < 0 ||
		newfd >= SCHED_FILE_MAX)
		return -EBADF;
	if (!process->files[oldfd])
		return -EBADF;
	if (flags & ~(SYS_O_CLOEXEC))
		return -EINVAL;
	if (oldfd == newfd)
		return newfd;

	if (process->files[newfd])
		syscall_close_file_slot(&process->files[newfd]);

	vfs_file_t *dup = syscall_dup_file(process->files[oldfd]);
	if (!dup)
		return -ENOMEM;

	process->files[newfd] = dup;
	process->fd_flags[newfd] = (flags & SYS_O_CLOEXEC) ? SYS_FD_CLOEXEC : 0;
	return newfd;
}

static long sys_pipe_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	if (!thread || !thread->process)
		return -EBADF;

	uint64_t user_fds = frame->rdi;
	int flags = (int)frame->rsi;
	pcb_t *process = thread->process;

	if (!user_fds || !syscall_user_range_ok(user_fds, sizeof(int) * 2))
		return -EINVAL;
	if (flags & ~(SYS_O_CLOEXEC | SYS_O_NONBLOCK))
		return -EINVAL;

	int read_fd = syscall_alloc_fd(process, 0);
	if (read_fd < 0)
		return read_fd;

	int write_fd = syscall_alloc_fd(process, read_fd + 1);
	if (write_fd < 0)
		return write_fd;

	vfs_file_t *read_end = NULL;
	vfs_file_t *write_end = NULL;
	int r = vfs_pipe_create(&read_end, &write_end);
	if (r != 0)
		return r;

	if (flags & SYS_O_NONBLOCK) {
		read_end->flags |= SYS_O_NONBLOCK;
		write_end->flags |= SYS_O_NONBLOCK;
	}

	process->files[read_fd] = read_end;
	process->files[write_fd] = write_end;
	process->fd_flags[read_fd] = (flags & SYS_O_CLOEXEC) ? SYS_FD_CLOEXEC : 0;
	process->fd_flags[write_fd] = (flags & SYS_O_CLOEXEC) ? SYS_FD_CLOEXEC : 0;

	((int *)(uintptr_t)user_fds)[0] = read_fd;
	((int *)(uintptr_t)user_fds)[1] = write_fd;
	return 0;
}

static long sys_stat_handler(interrupt_frame_t *frame)
{
	char path[SYS_PATH_MAX];
	int r = syscall_copy_user_path_abs(frame->rdi, path, sizeof(path));

	if (r != 0)
		return r;

	vfs_stat_t st;
	r = vfs_stat(path, syscall_current_cred(), &st);

	if (r != 0)
		return r;

	return syscall_copy_stat_out(frame->rsi, &st);
}

static long sys_fstat_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	int fd = (int)frame->rdi;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return -EBADF;

	vfs_file_t *file = thread->process->files[fd];

	if (!file || !file->node)
		return -EBADF;

	vfs_node_t *node = file->node;
	vfs_stat_t st;
	memset(&st, 0, sizeof(st));

	st.dev = node->dev;
	st.ino = node->ino;
	st.mode = node->mode;
	st.uid = node->uid;
	st.gid = node->gid;
	st.size = node->size;
	st.nlink = node->nlink;
	st.blksize = 4096;
	st.blocks = (node->size + 511) / 512;

	return syscall_copy_stat_out(frame->rsi, &st);
}

static long sys_lseek_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	int fd = (int)frame->rdi;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return -EBADF;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	if (VFS_S_ISFIFO(file->node->mode))
		return -ESPIPE;

	uint64_t new_off = 0;
	int r = vfs_seek(file, (int)frame->rdx, (int64_t)frame->rsi, &new_off);

	if (r != 0)
		return r;

	return (long)new_off;
}

static long sys_access_handler(interrupt_frame_t *frame)
{
	char path[SYS_PATH_MAX];
	int r = syscall_copy_user_path_abs(frame->rdi, path, sizeof(path));

	if (r != 0)
		return r;

	vfs_node_t *node = NULL;
	r = vfs_resolve(path, syscall_current_cred(), &node);

	if (r != 0)
		return r;

	r = vfs_access(node, syscall_current_cred(), (int)frame->rsi);
	vfs_node_release(node);

	return r;
}

static long sys_getdents_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	int fd = (int)frame->rdi;
	uint64_t user = frame->rsi;
	size_t cap = (size_t)frame->rdx;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return -EBADF;

	if (!user || cap == 0)
		return -EINVAL;

	if (!syscall_user_range_ok(user, cap))
		return -EINVAL;

	vfs_file_t *file = thread->process->files[fd];

	if (!file || !file->node)
		return -EBADF;

	if (!VFS_S_ISDIR(file->node->mode))
		return -ENOTDIR;

	size_t copied = 0;
	size_t base_size = sizeof(syscall_dirent_t);

	for (;;) {
		vfs_dirent_t ent;
		memset(&ent, 0, sizeof(ent));

		int r = vfs_readdir(file->node, (size_t)file->offset, &ent);

		if (r == -ENOENT)
			break;

		if (r != 0)
			return copied ? (long)copied : r;

		file->offset++;

		if (!ent.name[0])
			continue;

		size_t name_len = syscall_strnlen(ent.name, VFS_NAME_MAX);
		size_t reclen = syscall_align_up(base_size + name_len + 1, 8);

		if (reclen > cap)
			return -EINVAL;

		if (copied + reclen > cap) {
			file->offset--;
			break;
		}

		syscall_dirent_t *out = (syscall_dirent_t *)(user + copied);
		memset(out, 0, reclen);

		out->d_ino = file->offset;
		out->d_off = (int64_t)file->offset;
		out->d_reclen = (uint16_t)reclen;
		out->d_type = syscall_dirent_type(ent.mode);

		memcpy(out->d_name, ent.name, name_len);
		out->d_name[name_len] = '\0';

		copied += reclen;
	}

	return (long)copied;
}

/* -------------------------------------------------------------------------- */
/* Filesystem namespace syscalls                                              */
/* -------------------------------------------------------------------------- */

static long sys_chroot_handler(interrupt_frame_t *frame)
{
	char path[SYS_PATH_MAX];
	int r = syscall_copy_user_path_abs(frame->rdi, path, sizeof(path));

	if (r != 0)
		return r;

	return vfs_chroot(path, syscall_current_cred());
}

static long sys_mount_handler(interrupt_frame_t *frame)
{
	char source[256];
	char target_raw[SYS_PATH_MAX];
	char target[SYS_PATH_MAX];
	char fstype[32];

	int r = syscall_copy_user_string(frame->rdi, source, sizeof(source));

	if (r != 0)
		return r;

	r = syscall_copy_user_string(frame->rsi, target_raw, sizeof(target_raw));

	if (r != 0)
		return r;

	r = syscall_normalize_path(target_raw, target, sizeof(target));

	if (r != 0)
		return r;

	r = syscall_copy_user_string(frame->rdx, fstype, sizeof(fstype));

	if (r != 0)
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
	if (r != 0) {
		log_err(
			"syscall",
			"change_root: failed to copy source from user: status=%s(%d); exiting caller",
			errno_name(r), r);

		return (long)(uintptr_t)sched_syscall_exit(frame, r);
	}

	r = syscall_copy_user_string(frame->rsi, fstype, sizeof(fstype));
	if (r != 0) {
		log_err(
			"syscall",
			"change_root: failed to copy filesystem type from user: source='%s' status=%s(%d); exiting caller",
			source, errno_name(r), r);

		return (long)(uintptr_t)sched_syscall_exit(frame, r);
	}

	r = syscall_copy_user_string(frame->rdx, init_path, sizeof(init_path));
	if (r != 0) {
		log_err(
			"syscall",
			"change_root: failed to copy init path from user: source='%s' fstype='%s' status=%s(%d); exiting caller",
			source, fstype, errno_name(r), r);

		return (long)(uintptr_t)sched_syscall_exit(frame, r);
	}

	if (source[0] == '\0') {
		log_err("syscall", "change_root: empty source path; exiting caller");

		return (long)(uintptr_t)sched_syscall_exit(frame, -EINVAL);
	}

	if (fstype[0] == '\0') {
		log_err(
			"syscall",
			"change_root: empty filesystem type for source='%s'; exiting caller",
			source);

		return (long)(uintptr_t)sched_syscall_exit(frame, -EINVAL);
	}

	if (init_path[0] == '\0') {
		log_err(
			"syscall",
			"change_root: empty init path for source='%s' fstype='%s'; exiting caller",
			source, fstype);

		return (long)(uintptr_t)sched_syscall_exit(frame, -EINVAL);
	}

	r = vfs_mkdir("/newroot", 0755, syscall_current_cred());
	if (r != 0 && r != -EEXIST) {
		log_err(
			"syscall",
			"change_root: failed to create /newroot: status=%s(%d); exiting caller",
			errno_name(r), r);

		return (long)(uintptr_t)sched_syscall_exit(frame, r);
	}

	if (r == -EEXIST) {
		log_warn("syscall", "change_root: /newroot already exists, continuing");
	}

	r = fs_mount_spec(source, "/newroot", fstype, 0, NULL);
	if (r != 0) {
		log_err(
			"syscall",
			"change_root: failed to mount source='%s' type='%s' on /newroot: status=%s(%d); exiting caller",
			source, fstype, errno_name(r), r);

		return (long)(uintptr_t)sched_syscall_exit(frame, r);
	}

	r = vfs_change_root("/newroot", syscall_current_cred());
	if (r != 0) {
		log_err(
			"syscall",
			"change_root: mounted source='%s' type='%s' on /newroot but failed to switch root: status=%s(%d); exiting caller",
			source, fstype, errno_name(r), r);

		return (long)(uintptr_t)sched_syscall_exit(frame, r);
	}

	r = vfs_mkdir("/dev", 0755, syscall_current_cred());
	if (r != 0 && r != -EEXIST) {
		kpanic(
			frame,
			"change_root: new root is active, but failed to create /dev: source='%s' error=%s(%d)",
			source, errno_name(r), r);
	}

	r = fs_mount_spec("devfs", "/dev", "devfs", 0, NULL);
	if (r != 0) {
		kpanic(
			frame,
			"change_root: new root is active, but failed to mount devfs on /dev: source='%s' error=%s(%d)",
			source, errno_name(r), r);
	}

	r = init_spawn(init_path);
	if (r != 0) {
		kpanic(
			frame,
			"change_root: failed to launch init='%s' on new root from source='%s' type='%s': error=%s(%d)",
			init_path, source, fstype, errno_name(r), r);
	}

	log_info(
		"syscall",
		"change_root: successfully switched root to source='%s' and launched init='%s'",
		source, init_path);

	return (long)(uintptr_t)sched_syscall_exit(frame, 0);
}

static long sys_mkdir_handler(interrupt_frame_t *frame)
{
	char path[SYS_PATH_MAX];
	int r = syscall_copy_user_path_abs(frame->rdi, path, sizeof(path));

	if (r != 0)
		return r;

	return vfs_mkdir(path, (vfs_mode_t)frame->rsi, syscall_current_cred());
}

static long sys_rmdir_handler(interrupt_frame_t *frame)
{
	char path[SYS_PATH_MAX];
	int r = syscall_copy_user_path_abs(frame->rdi, path, sizeof(path));

	if (r != 0)
		return r;

	return vfs_rmdir(path, syscall_current_cred());
}

static long sys_unlink_handler(interrupt_frame_t *frame)
{
	char path[SYS_PATH_MAX];
	int r = syscall_copy_user_path_abs(frame->rdi, path, sizeof(path));

	if (r != 0)
		return r;

	return vfs_unlink(path, syscall_current_cred());
}

static long sys_renameat_handler(interrupt_frame_t *frame)
{
	if ((int)frame->rdi != SYS_AT_FDCWD || (int)frame->rdx != SYS_AT_FDCWD)
		return -ENOSYS;

	char old_path[SYS_PATH_MAX];
	char new_path[SYS_PATH_MAX];
	int r = syscall_copy_user_path_abs(frame->rsi, old_path, sizeof(old_path));
	if (r != 0)
		return r;

	r = syscall_copy_user_path_abs(frame->r10, new_path, sizeof(new_path));
	if (r != 0)
		return r;

	return vfs_rename(old_path, new_path, syscall_current_cred());
}

static long sys_chmod_handler(interrupt_frame_t *frame)
{
	char path[SYS_PATH_MAX];
	int r = syscall_copy_user_path_abs(frame->rdi, path, sizeof(path));

	if (r != 0)
		return r;

	return vfs_chmod(path, (vfs_mode_t)frame->rsi, syscall_current_cred());
}

static long sys_chown_handler(interrupt_frame_t *frame)
{
	char path[SYS_PATH_MAX];
	int r = syscall_copy_user_path_abs(frame->rdi, path, sizeof(path));

	if (r != 0)
		return r;

	return vfs_chown(path, (vfs_uid_t)frame->rsi, (vfs_gid_t)frame->rdx,
					 syscall_current_cred());
}

static long sys_chdir_handler(interrupt_frame_t *frame)
{
	char path[SYS_PATH_MAX];
	int r = syscall_copy_user_path_abs(frame->rdi, path, sizeof(path));

	if (r != 0)
		return r;

	vfs_node_t *node = NULL;
	r = vfs_resolve(path, syscall_current_cred(), &node);

	if (r != 0)
		return r;

	if (!VFS_S_ISDIR(node->mode)) {
		vfs_node_release(node);
		return -ENOTDIR;
	}

	r = vfs_access(node, syscall_current_cred(), VFS_X_OK);
	vfs_node_release(node);

	if (r != 0)
		return r;

	return sched_process_setcwd(syscall_current_process(), path);
}

static long sys_getcwd_handler(interrupt_frame_t *frame)
{
	uint64_t user = frame->rdi;
	size_t len = (size_t)frame->rsi;
	pcb_t *process = syscall_current_process();
	const char *cwd = sched_process_cwd(process);
	size_t needed = strlen(cwd) + 1;

	if (!user || len == 0 || !syscall_user_range_ok(user, len))
		return -EINVAL;

	if (len < needed)
		return -ENAMETOOLONG;

	memcpy((void *)user, cwd, needed);
	return (long)needed;
}

/* -------------------------------------------------------------------------- */
/* Process and VM syscalls                                                    */
/* -------------------------------------------------------------------------- */

static const char *exec_comm_from_path(const char *path)
{
	const char *base = path;

	if (!path)
		return "process";

	for (const char *p = path; *p; p++) {
		if (*p == '/')
			base = p + 1;
	}

	return base[0] ? base : path;
}

static long sys_execve_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();

	if (!thread || !thread->process || !thread->process->vas)
		return -EBADF;

	char path[SYS_PATH_MAX];
	int r = syscall_copy_user_path_abs(frame->rdi, path, sizeof(path));

	if (r != 0)
		return r;

	uint64_t argv_ptrs[EXEC_ARG_MAX + 1];
	uint64_t env_ptrs[EXEC_ARG_MAX + 1];
	char arg_bufs[EXEC_ARG_MAX][EXEC_STR_MAX];
	char env_bufs[EXEC_ARG_MAX][EXEC_STR_MAX];
	size_t argc = 0;
	size_t envc = 0;

	r = syscall_copy_user_ptrs(frame->rsi, argv_ptrs, EXEC_ARG_MAX + 1, &argc);

	if (r != 0)
		return r;

	if (argc > EXEC_ARG_MAX)
		return -ENAMETOOLONG;

	r = syscall_copy_user_ptrs(frame->rdx, env_ptrs, EXEC_ARG_MAX + 1, &envc);

	if (r != 0)
		return r;

	if (envc > EXEC_ARG_MAX)
		return -ENAMETOOLONG;

	char *argv[EXEC_ARG_MAX];
	char *envp[EXEC_ARG_MAX];

	for (size_t i = 0; i < argc; i++) {
		r = syscall_copy_user_string(argv_ptrs[i], arg_bufs[i],
									 sizeof(arg_bufs[i]));

		if (r != 0)
			return r;

		argv[i] = arg_bufs[i];
	}

	for (size_t i = 0; i < envc; i++) {
		r = syscall_copy_user_string(env_ptrs[i], env_bufs[i],
									 sizeof(env_bufs[i]));

		if (r != 0)
			return r;

		envp[i] = env_bufs[i];
	}

	vas_t *new_vas = vas_create(NULL);

	if (!new_vas)
		return -ENOMEM;

	elf_user_image_t image;
	r = elf_load_user_executable(new_vas, path, &image);

	if (r != 0) {
		vas_destroy(new_vas);
		return r;
	}

	uint64_t stack_top = 0x00007ffffff000ULL;
	uint64_t stack_size = SYS_USER_STACK_SIZE;
	uint64_t stack_guard = SYS_USER_STACK_GUARD_SIZE;
	uint64_t stack_base = stack_top - (stack_size + stack_guard);
	uint64_t stack_map_base = stack_base + stack_guard;

	if (vas_map_anon(new_vas, stack_map_base, stack_size,
					 VMM_PRESENT | VMM_WRITABLE | VMM_USER | VMM_NX |
						 VAD_FIXED) != stack_map_base) {
		vas_destroy(new_vas);
		return -ENOMEM;
	}

	uint64_t user_rsp = 0;
	r = elf_build_initial_stack(
		new_vas, stack_top, path, (const char *const *)argv, argc,
		(const char *const *)envp, envc, &image, &user_rsp);

	if (r != 0) {
		vas_destroy(new_vas);
		return r;
	}

	log_debug(
		"execve",
		"path=%s entry=0x%llx prog_entry=0x%llx phdr=0x%llx phnum=%u rsp=0x%llx",
		path, image.entry, image.program_entry, image.program_phdr,
		image.program_phnum, user_rsp);

	const char *comm = exec_comm_from_path(path);

	vas_t *old_vas = thread->process->vas;
	thread->process->vas = new_vas;
	thread->process->pml4 = new_vas->pml4;
	sched_process_set_name(thread->process, comm);
	sched_thread_set_name(thread, comm);
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

	return 0;
}

static long sys_arch_prctl_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();

	if (!thread)
		return -EINVAL;

	switch ((long)frame->rdi) {
	case ARCH_SET_FS:
		if (frame->rsi >= VAS_USER_END)
			return -EINVAL;

		thread->fs_base = frame->rsi;
		write_fs_base(frame->rsi);
		return 0;

	case ARCH_GET_FS:
		return syscall_copy_to_user(frame->rsi, &thread->fs_base,
									sizeof(thread->fs_base));

	default:
		return -ENOSYS;
	}
}

static long sys_mmap_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();

	if (!thread || !thread->process || !thread->process->vas)
		return -EBADF;

	uint64_t addr = frame->rdi;
	size_t length = (size_t)frame->rsi;
	int prot = (int)frame->rdx;
	int flags = (int)frame->r10;
	int fd = (int)frame->r8;
	uint64_t offset = frame->r9;
	int shared = (flags & SYS_MAP_SHARED) != 0;
	int private = (flags & SYS_MAP_PRIVATE) != 0;
	int anonymous = (flags & SYS_MAP_ANONYMOUS) != 0;
	int fixed = (flags & SYS_MAP_FIXED) != 0;
	vfs_file_t *file = NULL;

	if (!length)
		return -EINVAL;
	if (shared == private)
		return -EINVAL;
	if ((offset & (PAGE_SIZE - 1)) != 0)
		return -EINVAL;
	if (fixed && (addr & (PAGE_SIZE - 1)) != 0)
		return -EINVAL;

	uint64_t vmm_flags = syscall_mmap_prot_to_vmm(prot);

	if (fixed)
		vmm_flags |= VAD_FIXED;
	if (shared)
		vmm_flags |= VAD_SHARED;

	if (!anonymous) {
		if (fd < 0 || fd >= SCHED_FILE_MAX)
			return -EBADF;
		file = thread->process->files[fd];
		if (!file)
			return -EBADF;

		uint32_t accmode = file->flags & VFS_O_ACCMODE;
		if (private) {
			if (accmode == VFS_O_WRONLY)
				return -EACCES;
		} else {
			if ((prot & SYS_PROT_READ) && accmode == VFS_O_WRONLY)
				return -EACCES;
			if ((prot & SYS_PROT_WRITE) && accmode == VFS_O_RDONLY)
				return -EACCES;
		}
	}

	if (fixed)
		vas_unmap(thread->process->vas, addr, length);

	if (anonymous) {
		uint64_t mapped =
			vas_map_anon(thread->process->vas, addr, length, vmm_flags);

		if (!mapped)
			return -ENOMEM;

		return (long)mapped;
	}

	if (shared) {
		vfs_node_t *node = vfs_file_node(file);
		if (!node || !node->ops || (!node->ops->mmap && !node->ops->get_page))
			return -ENOSYS;

		uint64_t mapped = vfs_mmap(file, thread->process->vas, addr, offset,
								   length, vmm_flags);

		if (!mapped)
			return -ENOMEM;

		return (long)mapped;
	}

	uint64_t mapped =
		vas_map_anon(thread->process->vas, addr, length, vmm_flags);

	if (!mapped)
		return -ENOMEM;

	int r = syscall_mmap_fill_private(file, mapped, length, offset);
	if (r != 0) {
		vas_unmap(thread->process->vas, mapped, length);
		return r;
	}

	return (long)mapped;
}

static long sys_munmap_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();

	if (!thread || !thread->process || !thread->process->vas)
		return -EBADF;

	return vas_unmap(thread->process->vas, frame->rdi, (size_t)frame->rsi);
}

static long sys_mprotect_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();

	if (!thread || !thread->process || !thread->process->vas)
		return -EBADF;

	if (!vas_range_mapped(thread->process->vas, frame->rdi, (size_t)frame->rsi))
		return -ENOMEM;

	return vas_protect(thread->process->vas, frame->rdi, (size_t)frame->rsi,
					   syscall_mmap_prot_to_vmm((int)frame->rdx));
}

static long sys_exit_handler(interrupt_frame_t *frame)
{
	return (long)(uintptr_t)sched_syscall_exit(frame, (int)frame->rdi);
}

static long sys_waitpid_handler(interrupt_frame_t *frame)
{
	pcb_t *process = syscall_current_process();
	pid_t pid = (pid_t)frame->rdi;
	uint64_t status_user = frame->rsi;
	int options = (int)frame->rdx;

	if (!process)
		return -EBADF;

	if (status_user && !syscall_user_range_write_ok(status_user, sizeof(int)))
		return -EFAULT;

	/* Map Linux WNOHANG/WUNTRACED/WCONTINUED to our internal flags */
	int kern_opts = 0;
	if (options & 1)
		kern_opts |= SCHED_WNOHANG;
	if (options & 2)
		kern_opts |= SCHED_WUNTRACED;
	if (options & 8)
		kern_opts |= SCHED_WCONTINUED;
	/* Reject unknown bits */
	if (options & ~(1 | 2 | 8))
		return -EINVAL;

	for (;;) {
		pid_t waited = 0;
		int status = 0;

		/* Snapshot child_event before the poll so we can detect
		 * new events that arrive between the poll returning -EAGAIN
		 * and us going to sleep. */
		unsigned ev_before =
			atomic_load_explicit(&process->child_event, memory_order_acquire);

		int r = sched_process_wait(process, pid, kern_opts, &waited, &status);

		if (r == 0) {
			if (status_user && waited != 0) {
				r = syscall_copy_to_user(status_user, &status, sizeof(status));
				if (r != 0)
					return r;
			}
			return waited;
		}

		if (r == -ECHILD)
			return r;

		if (r != -EAGAIN)
			return r;

		/* WNOHANG: return 0 immediately */
		if (kern_opts & SCHED_WNOHANG) {
			if (status_user) {
				int zero = 0;
				syscall_copy_to_user(status_user, &zero, sizeof(zero));
			}
			return 0;
		}

		/* Check for a non-SIGCHLD signal that should interrupt us */
		if (syscall_interrupted())
			return -EINTR;

		/* Sleep until a child changes state.  We use a short finite
		 * timeout so that events posted between the poll and sleep are
		 * not missed; the child_event counter is the authoritative
		 * wakeup source. */
		time_timeout_t timeout;
		time_timeout_after_ms(50, &timeout);
		time_sleep_until_interrupt_or_timeout(&timeout);

		/* Woken by timer or interrupt: check for a real signal first */
		if (syscall_interrupted())
			return -EINTR;

		/* If child_event didn't advance and timeout didn't fire yet,
		 * keep waiting; otherwise loop back and re-poll. */
		unsigned ev_after =
			atomic_load_explicit(&process->child_event, memory_order_acquire);
		(void)ev_before;
		(void)ev_after;
		/* Always loop: sched_process_wait is cheap */
	}
}

static long sys_fork_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();

	if (!thread || !thread->process || !thread->process->vas)
		return -EBADF;

	vas_t *child_vas = vas_clone(thread->process->vas);

	if (!child_vas)
		return -ENOMEM;

	pcb_t *child = sched_process_create(thread->process->name, child_vas);

	if (!child) {
		vas_destroy(child_vas);
		return -ENOMEM;
	}

	sched_process_copy_ids(child, thread->process);
	sched_process_copy_cwd(child, thread->process);

	for (int i = 0; i < SCHED_FILE_MAX; i++)
		syscall_close_file_slot(&child->files[i]);

	for (int i = 0; i < SCHED_FILE_MAX; i++) {
		if (!thread->process->files[i])
			continue;

		child->files[i] = syscall_dup_file(thread->process->files[i]);

		if (!child->files[i]) {
			for (int j = 0; j < SCHED_FILE_MAX; j++)
				syscall_close_file_slot(&child->files[j]);

			sched_process_discard(child);
			return -ENOMEM;
		}

		child->fd_flags[i] = thread->process->fd_flags[i];
	}

	tcb_t *child_thread = sched_fork_thread(child, thread->name, frame);

	if (!child_thread) {
		for (int i = 0; i < SCHED_FILE_MAX; i++)
			syscall_close_file_slot(&child->files[i]);

		sched_process_discard(child);
		return -ENOMEM;
	}

	child_thread->fs_base = thread->fs_base;

	return (long)child->pid;
}

/* -------------------------------------------------------------------------- */
/* Misc/device syscalls                                                       */
/* -------------------------------------------------------------------------- */

static long sys_uname_handler(interrupt_frame_t *frame)
{
	uint64_t user = frame->rdi;

	if (!user || !syscall_user_range_ok(user, sizeof(syscall_utsname_t)))
		return -EINVAL;

	syscall_utsname_t uts;
	memset(&uts, 0, sizeof(uts));

	syscall_uts_copy(uts.sysname, "lyrOS");
	syscall_uts_copy(uts.nodename, "lyr");
	syscall_uts_copy(uts.release, "1.0-alpha (mlibc)");
	syscall_uts_copyf(uts.version, "lyr-%s", LYR_VERSION);

#if defined(__x86_64__)
	syscall_uts_copy(uts.machine, "x86_64");
#else
	syscall_uts_copy(uts.machine, "unknown");
#endif

	syscall_uts_copy(uts.domainname, "localdomain");

	memcpy((void *)user, &uts, sizeof(uts));
	return 0;
}

static long sys_ioctl_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	int fd = (int)frame->rdi;
	unsigned long request = (unsigned long)frame->rsi;
	void *arg = (void *)frame->rdx;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return -EBADF;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	return vfs_ioctl(file, request, arg);
}

static long sys_clock_get_handler(interrupt_frame_t *frame)
{
	int clock_id = (int)frame->rdi;
	uint64_t user_ts = frame->rsi;

	if (!user_ts)
		return -EINVAL;

	int64_t sec = 0;
	long nsec = 0;
	int r = time_get(clock_id, &sec, &nsec);

	if (r != 0)
		return r;

	if (nsec < 0 || nsec >= SYS_NSEC_PER_SEC)
		return -EINVAL;

	syscall_timespec_t ts = {
		.tv_sec = sec,
		.tv_nsec = nsec,
	};

	return syscall_copy_to_user(user_ts, &ts, sizeof(ts));
}

/* -------------------------------------------------------------------------- */
/* Socket syscalls                                                            */
/* -------------------------------------------------------------------------- */

static long sys_socket_handler(interrupt_frame_t *frame)
{
	int domain = (int)frame->rdi;
	int type = (int)frame->rsi;
	int protocol = (int)frame->rdx;

	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	socket_t *sock = NULL;
	int r = net_socket(&sock, domain, type, protocol);

	if (r != NET_SOCK_OK)
		return r;

	for (int fd = 0; fd < SCHED_FILE_MAX; fd++) {
		if (!thread->process->files[fd]) {
			vfs_file_t *file = kzalloc(sizeof(*file));

			if (!file) {
				net_close(sock);
				return -ENOMEM;
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
	return -ENOMEM;
}

static long sys_bind_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	uint64_t addr_ptr = frame->rsi;
	socklen_t addrlen = (socklen_t)frame->rdx;

	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return -EBADF;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	socket_t *sock = file->private_data;

	if (!sock)
		return -EBADF;

	if (!addr_ptr || !syscall_user_range_ok(addr_ptr, addrlen))
		return -EINVAL;

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

	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return -EBADF;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	socket_t *sock = file->private_data;

	if (!sock)
		return -EBADF;

	if (!addr_ptr || !syscall_user_range_ok(addr_ptr, addrlen))
		return -EINVAL;

	sockaddr_t addr;

	if (addrlen > sizeof(addr))
		addrlen = sizeof(addr);

	memcpy(&addr, (void *)addr_ptr, addrlen);

	return net_connect(sock, &addr, addrlen);
}

static long sys_listen_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	int backlog = (int)frame->rsi;

	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return -EBADF;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	socket_t *sock = file->private_data;

	if (!sock)
		return -EBADF;

	return net_listen(sock, backlog);
}

static long sys_accept_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	uint64_t addr_ptr = frame->rsi;
	uint64_t addrlen_ptr = frame->rdx;

	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return -EBADF;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	socket_t *sock = file->private_data;

	if (!sock)
		return -EBADF;

	socket_t *client = NULL;
	sockaddr_t addr;
	socklen_t addrlen = 0;

	if (addrlen_ptr && syscall_user_range_ok(addrlen_ptr, sizeof(socklen_t)))
		addrlen = *(socklen_t *)addrlen_ptr;

	int r = net_accept(sock, &client, &addr, &addrlen);

	if (r != NET_SOCK_OK)
		return r;

	for (int newfd = 0; newfd < SCHED_FILE_MAX; newfd++) {
		if (!thread->process->files[newfd]) {
			vfs_file_t *newfile = kzalloc(sizeof(*newfile));

			if (!newfile) {
				net_close(client);
				return -ENOMEM;
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
	return -ENOMEM;
}

static long sys_send_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	uint64_t buf_ptr = frame->rsi;
	size_t len = (size_t)frame->rdx;
	int flags = (int)frame->r10;

	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX || !buf_ptr ||
		!syscall_user_range_ok(buf_ptr, len))
		return -EINVAL;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	socket_t *sock = file->private_data;

	if (!sock)
		return -EBADF;

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
		return -EBADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX || !buf_ptr ||
		!syscall_user_range_ok(buf_ptr, len))
		return -EINVAL;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	socket_t *sock = file->private_data;

	if (!sock)
		return -EBADF;

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
		return -EBADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX || !buf_ptr ||
		!syscall_user_range_ok(buf_ptr, len))
		return -EINVAL;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	socket_t *sock = file->private_data;

	if (!sock)
		return -EBADF;

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
		return -EBADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX || !buf_ptr ||
		!syscall_user_range_ok(buf_ptr, len))
		return -EINVAL;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	socket_t *sock = file->private_data;

	if (!sock)
		return -EBADF;

	sockaddr_t addr;
	socklen_t addrlen = 0;

	if (addr_ptr && addrlen_ptr &&
		syscall_user_range_ok(addrlen_ptr, sizeof(socklen_t)))
		addrlen = *(socklen_t *)addrlen_ptr;

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
		return -EBADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return -EBADF;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	socket_t *sock = file->private_data;

	if (!sock)
		return -EBADF;

	return net_shutdown(sock, how);
}

static long sys_getsockname_handler(interrupt_frame_t *frame)
{
	int fd = (int)frame->rdi;
	uint64_t addr_ptr = frame->rsi;
	uint64_t addrlen_ptr = frame->rdx;

	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return -EBADF;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	socket_t *sock = file->private_data;

	if (!sock)
		return -EBADF;

	socklen_t addrlen = 0;

	if (addrlen_ptr && syscall_user_range_ok(addrlen_ptr, sizeof(socklen_t)))
		addrlen = *(socklen_t *)addrlen_ptr;

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
		return -EBADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return -EBADF;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	socket_t *sock = file->private_data;

	if (!sock)
		return -EBADF;

	socklen_t addrlen = 0;

	if (addrlen_ptr && syscall_user_range_ok(addrlen_ptr, sizeof(socklen_t)))
		addrlen = *(socklen_t *)addrlen_ptr;

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
		return -EBADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return -EBADF;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	socket_t *sock = file->private_data;

	if (!sock)
		return -EBADF;

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
		return -EBADF;

	if (fd < 0 || fd >= SCHED_FILE_MAX)
		return -EBADF;

	vfs_file_t *file = thread->process->files[fd];

	if (!file)
		return -EBADF;

	socket_t *sock = file->private_data;

	if (!sock)
		return -EBADF;

	socklen_t optlen = 0;

	if (optlen_ptr && syscall_user_range_ok(optlen_ptr, sizeof(socklen_t)))
		optlen = *(socklen_t *)optlen_ptr;

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

/* -------------------------------------------------------------------------- */
/* Poll/select syscalls                                                       */
/* -------------------------------------------------------------------------- */

static int sys_poll_scan(struct lyr_pollfd *fds, size_t nfds)
{
	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

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
			int pr = vfs_poll(file, fds[i].events);

			if (pr < 0)
				fds[i].revents = LYR_POLLERR;
			else
				fds[i].revents = (int16_t)pr;
		}

		if (fds[i].revents)
			ready++;
	}

	return ready;
}

static long sys_poll_handler(interrupt_frame_t *frame)
{
	uint64_t user_fds = frame->rdi;
	size_t nfds = (size_t)frame->rsi;
	int timeout_ms = (int)frame->rdx;

	if (nfds > SYS_POLL_NFDS_MAX)
		return -EINVAL;

	if (nfds &&
		!syscall_user_range_ok(user_fds, nfds * sizeof(struct lyr_pollfd)))
		return -EFAULT;

	struct lyr_pollfd *fds = NULL;

	if (nfds) {
		fds = kzalloc(nfds * sizeof(*fds));

		if (!fds)
			return -ENOMEM;

		if (syscall_copy_from_user(fds, user_fds, nfds * sizeof(*fds)) != 0) {
			kfree(fds);
			return -EFAULT;
		}
	}

	time_timeout_t timeout;
	int tr = time_timeout_after_ms(timeout_ms, &timeout);
	if (tr != 0) {
		if (fds)
			kfree(fds);
		return tr;
	}

	long ret = 0;

	for (;;) {
		ret = nfds ? sys_poll_scan(fds, nfds) : 0;

		if (ret < 0 || ret > 0)
			break;

		if (time_timeout_expired(&timeout))
			break;

		time_sleep_until_interrupt_or_timeout(&timeout);
		if (syscall_interrupted() != 0) {
			ret = -EINTR;
			break;
		}
	}

	if (nfds && ret >= 0) {
		if (syscall_copy_to_user(user_fds, fds, nfds * sizeof(*fds)) != 0)
			ret = -EFAULT;
	}

	if (fds)
		kfree(fds);

	return ret;
}

static int sys_pselect_set_has(const uint8_t *set, int fd)
{
	return set[fd / 8] & (uint8_t)(1u << (fd % 8));
}

static void sys_pselect_set_add(uint8_t *set, int fd)
{
	set[fd / 8] |= (uint8_t)(1u << (fd % 8));
}

static long sys_pselect_handler(interrupt_frame_t *frame)
{
	int nfds = (int)frame->rdi;
	uint64_t readfds_ptr = frame->rsi;
	uint64_t writefds_ptr = frame->rdx;
	uint64_t exceptfds_ptr = frame->r10;
	uint64_t timeout_ptr = frame->r8;
	uint64_t sigmask_ptr = frame->r9;

	(void)sigmask_ptr;

	if (nfds < 0 || nfds > SYS_POLL_NFDS_MAX || nfds > SCHED_FILE_MAX)
		return -EINVAL;

	size_t set_bytes = ((size_t)nfds + 7) / 8;

	if (readfds_ptr && !syscall_user_range_ok(readfds_ptr, set_bytes))
		return -EFAULT;

	if (writefds_ptr && !syscall_user_range_ok(writefds_ptr, set_bytes))
		return -EFAULT;

	if (exceptfds_ptr && !syscall_user_range_ok(exceptfds_ptr, set_bytes))
		return -EFAULT;

	time_timeout_t timeout;
	int tr = syscall_copy_timespec_timeout(timeout_ptr, &timeout);

	if (tr != 0)
		return tr;

	tcb_t *thread = sched_current();

	if (!thread || !thread->process)
		return -EBADF;

	struct lyr_pollfd *fds = NULL;
	uint8_t *readfds = NULL;
	uint8_t *writefds = NULL;
	uint8_t *exceptfds = NULL;

	if (set_bytes) {
		if (readfds_ptr) {
			readfds = kzalloc(set_bytes);
			if (!readfds)
				return -ENOMEM;
			if (syscall_copy_from_user(readfds, readfds_ptr, set_bytes) != 0) {
				kfree(readfds);
				return -EFAULT;
			}
		}

		if (writefds_ptr) {
			writefds = kzalloc(set_bytes);
			if (!writefds) {
				kfree(readfds);
				return -ENOMEM;
			}
			if (syscall_copy_from_user(writefds, writefds_ptr, set_bytes) !=
				0) {
				kfree(writefds);
				kfree(readfds);
				return -EFAULT;
			}
		}

		if (exceptfds_ptr) {
			exceptfds = kzalloc(set_bytes);
			if (!exceptfds) {
				kfree(writefds);
				kfree(readfds);
				return -ENOMEM;
			}
			if (syscall_copy_from_user(exceptfds, exceptfds_ptr, set_bytes) !=
				0) {
				kfree(exceptfds);
				kfree(writefds);
				kfree(readfds);
				return -EFAULT;
			}
		}
	}

	if (nfds > 0) {
		fds = kzalloc((size_t)nfds * sizeof(*fds));

		if (!fds) {
			kfree(exceptfds);
			kfree(writefds);
			kfree(readfds);
			return -ENOMEM;
		}

		for (int i = 0; i < nfds; i++) {
			fds[i].fd = -1;
			fds[i].events = 0;
			fds[i].revents = 0;

			if (readfds && sys_pselect_set_has(readfds, i)) {
				fds[i].fd = i;
				fds[i].events |= LYR_POLLIN | LYR_POLLRDNORM;
			}

			if (writefds && sys_pselect_set_has(writefds, i)) {
				fds[i].fd = i;
				fds[i].events |= LYR_POLLOUT | LYR_POLLWRNORM;
			}

			if (exceptfds && sys_pselect_set_has(exceptfds, i)) {
				fds[i].fd = i;
				fds[i].events |= LYR_POLLPRI | LYR_POLLRDBAND;
			}
		}
	}

	long ready = 0;

	for (;;) {
		ready = nfds ? sys_poll_scan(fds, (size_t)nfds) : 0;

		if (ready < 0 || ready > 0)
			break;

		if (time_timeout_expired(&timeout))
			break;

		time_sleep_until_interrupt_or_timeout(&timeout);
		if (syscall_interrupted() != 0) {
			ready = -EINTR;
			break;
		}
	}

	if (ready >= 0) {
		if (readfds)
			memset(readfds, 0, set_bytes);

		if (writefds)
			memset(writefds, 0, set_bytes);

		if (exceptfds)
			memset(exceptfds, 0, set_bytes);

		if (fds) {
			for (int i = 0; i < nfds; i++) {
				if (fds[i].fd < 0 || !fds[i].revents)
					continue;

				if (readfds && (fds[i].revents &
								(LYR_POLLIN | LYR_POLLRDNORM | LYR_POLLHUP |
								 LYR_POLLERR | LYR_POLLNVAL))) {
					sys_pselect_set_add(readfds, i);
				}

				if (writefds && (fds[i].revents &
								 (LYR_POLLOUT | LYR_POLLWRNORM | LYR_POLLHUP |
								  LYR_POLLERR | LYR_POLLNVAL))) {
					sys_pselect_set_add(writefds, i);
				}

				if (exceptfds &&
					(fds[i].revents & (LYR_POLLPRI | LYR_POLLRDBAND |
									   LYR_POLLERR | LYR_POLLNVAL))) {
					sys_pselect_set_add(exceptfds, i);
				}
			}
		}

		if (readfds_ptr &&
			syscall_copy_to_user(readfds_ptr, readfds, set_bytes) != 0)
			ready = -EFAULT;
		if (ready >= 0 && writefds_ptr &&
			syscall_copy_to_user(writefds_ptr, writefds, set_bytes) != 0)
			ready = -EFAULT;
		if (ready >= 0 && exceptfds_ptr &&
			syscall_copy_to_user(exceptfds_ptr, exceptfds, set_bytes) != 0)
			ready = -EFAULT;
	}

	if (fds)
		kfree(fds);
	if (exceptfds)
		kfree(exceptfds);
	if (writefds)
		kfree(writefds);
	if (readfds)
		kfree(readfds);

	return ready;
}

/* -------------------------------------------------------------------------- */
/* Identity syscalls                                                          */
/* -------------------------------------------------------------------------- */

static long sys_getpid_handler(interrupt_frame_t *frame)
{
	(void)frame;

	pcb_t *process = syscall_current_process();

	if (!process)
		return -EBADF;

	return (long)process->pid;
}

static long sys_getppid_handler(interrupt_frame_t *frame)
{
	(void)frame;

	pcb_t *process = syscall_current_process();

	if (!process)
		return -EBADF;

	return (long)process->ppid;
}

static long sys_gettid_handler(interrupt_frame_t *frame)
{
	(void)frame;

	tcb_t *thread = sched_current();

	if (!thread)
		return -EBADF;

	return (long)thread->tid;
}

static long sys_getuid_handler(interrupt_frame_t *frame)
{
	(void)frame;

	pcb_t *process = syscall_current_process();

	if (!process)
		return -EBADF;

	return (long)process->ruid;
}

static long sys_geteuid_handler(interrupt_frame_t *frame)
{
	(void)frame;

	pcb_t *process = syscall_current_process();

	if (!process)
		return -EBADF;

	return (long)process->euid;
}

static long sys_getgid_handler(interrupt_frame_t *frame)
{
	(void)frame;

	pcb_t *process = syscall_current_process();

	if (!process)
		return -EBADF;

	return (long)process->rgid;
}

static long sys_getegid_handler(interrupt_frame_t *frame)
{
	(void)frame;

	pcb_t *process = syscall_current_process();

	if (!process)
		return -EBADF;

	return (long)process->egid;
}

static long sys_getresuid_handler(interrupt_frame_t *frame)
{
	pcb_t *process = syscall_current_process();

	if (!process)
		return -EBADF;

	if (frame->rdi) {
		int r = syscall_copy_to_user(frame->rdi, &process->ruid,
									 sizeof(process->ruid));
		if (r != 0)
			return r;
	}

	if (frame->rsi) {
		int r = syscall_copy_to_user(frame->rsi, &process->euid,
									 sizeof(process->euid));
		if (r != 0)
			return r;
	}

	if (frame->rdx) {
		int r = syscall_copy_to_user(frame->rdx, &process->suid,
									 sizeof(process->suid));
		if (r != 0)
			return r;
	}

	return 0;
}

static long sys_getresgid_handler(interrupt_frame_t *frame)
{
	pcb_t *process = syscall_current_process();

	if (!process)
		return -EBADF;

	if (frame->rdi) {
		int r = syscall_copy_to_user(frame->rdi, &process->rgid,
									 sizeof(process->rgid));
		if (r != 0)
			return r;
	}

	if (frame->rsi) {
		int r = syscall_copy_to_user(frame->rsi, &process->egid,
									 sizeof(process->egid));
		if (r != 0)
			return r;
	}

	if (frame->rdx) {
		int r = syscall_copy_to_user(frame->rdx, &process->sgid,
									 sizeof(process->sgid));
		if (r != 0)
			return r;
	}

	return 0;
}

static long sys_getgroups_handler(interrupt_frame_t *frame)
{
	pcb_t *process = syscall_current_process();
	size_t size = (size_t)frame->rdi;
	size_t count;

	if (!process)
		return -EBADF;

	count = process->cred.group_count;
	if (count > VFS_SUPP_GROUP_MAX)
		count = VFS_SUPP_GROUP_MAX;

	if (size == 0)
		return (long)count;

	if (size < count)
		return -EINVAL;

	if (!frame->rsi && count)
		return -EFAULT;

	if (count) {
		int r = syscall_copy_to_user(frame->rsi, process->cred.groups,
									 count * sizeof(process->cred.groups[0]));
		if (r != 0)
			return r;
	}

	return (long)count;
}

static long sys_setuid_handler(interrupt_frame_t *frame)
{
	return sched_process_setuid(syscall_current_process(),
								(vfs_uid_t)frame->rdi);
}

static long sys_seteuid_handler(interrupt_frame_t *frame)
{
	return sched_process_seteuid(syscall_current_process(),
								 (vfs_uid_t)frame->rdi);
}

static long sys_setgid_handler(interrupt_frame_t *frame)
{
	return sched_process_setgid(syscall_current_process(),
								(vfs_gid_t)frame->rdi);
}

static long sys_setegid_handler(interrupt_frame_t *frame)
{
	return sched_process_setegid(syscall_current_process(),
								 (vfs_gid_t)frame->rdi);
}

static long sys_kill_handler(interrupt_frame_t *frame)
{
	pcb_t *process = syscall_current_process();
	pid_t pid = (pid_t)frame->rdi;
	int signal = (int)frame->rsi;

	if (!process)
		return -EBADF;

	return sched_process_signal(process, pid, signal);
}

static long sys_getpgid_handler(interrupt_frame_t *frame)
{
	pid_t pgid = 0;
	int r = sched_process_getpgid(syscall_current_process(), (pid_t)frame->rdi,
								  &pgid);
	if (r != 0)
		return r;
	return pgid;
}

static long sys_setpgid_handler(interrupt_frame_t *frame)
{
	return sched_process_setpgid(syscall_current_process(), (pid_t)frame->rdi,
								 (pid_t)frame->rsi);
}

static long sys_setsid_handler(interrupt_frame_t *frame)
{
	pid_t sid = 0;
	int r = sched_process_setsid(syscall_current_process(), &sid);
	if (r != 0)
		return r;
	return sid;
}

static long sys_sigaction_handler(interrupt_frame_t *frame)
{
	pcb_t *process = syscall_current_process();
	int signal = (int)frame->rdi;
	uint64_t act_user = frame->rsi;
	uint64_t old_user = frame->rdx;

	if (!process)
		return -EBADF;

	sched_sigaction_t act;
	sched_sigaction_t oldact;
	sched_sigaction_t *actp = NULL;
	sched_sigaction_t *oldp = old_user ? &oldact : NULL;

	if (act_user) {
		syscall_sigaction_t u;
		int cr = syscall_copy_from_user(&u, act_user, sizeof(u));
		if (cr != 0)
			return cr;
		act.handler = u.handler;
		act.flags = u.flags;
		act.restorer = u.restorer;
		act.mask = u.mask;
		actp = &act;
	}

	int r = sched_signal_action(process, signal, actp, oldp);
	if (r != 0)
		return r;

	if (old_user) {
		syscall_sigaction_t u = {
			.handler = oldact.handler,
			.flags = oldact.flags,
			.restorer = oldact.restorer,
			.mask = oldact.mask,
		};
		r = syscall_copy_to_user(old_user, &u, sizeof(u));
		if (r != 0)
			return r;
	}

	return 0;
}

static long sys_sigprocmask_handler(interrupt_frame_t *frame)
{
	tcb_t *thread = sched_current();
	int how = (int)frame->rdi;
	uint64_t set_user = frame->rsi;
	uint64_t old_user = frame->rdx;
	uint64_t set;
	uint64_t oldset;
	uint64_t *setp = NULL;
	uint64_t *oldp = old_user ? &oldset : NULL;

	if (!thread)
		return -EBADF;

	if (set_user) {
		int cr = syscall_copy_from_user(&set, set_user, sizeof(set));
		if (cr != 0)
			return cr;
		setp = &set;
	}

	int r = sched_signal_procmask(thread, how, setp, oldp);
	if (r != 0)
		return r;

	if (old_user) {
		r = syscall_copy_to_user(old_user, &oldset, sizeof(oldset));
		if (r != 0)
			return r;
	}

	return 0;
}

static long sys_sigreturn_handler(interrupt_frame_t *frame)
{
	return (long)(uintptr_t)sched_signal_return(frame);
}

/* -------------------------------------------------------------------------- */
/* Sleep syscall                                                              */
/* -------------------------------------------------------------------------- */
static long sys_nsleep_handler(interrupt_frame_t *frame)
{
	int64_t sec = (int64_t)frame->rdi;
	long nsec = (long)frame->rsi;

	if (sec < 0 || nsec < 0 || nsec >= SYS_NSEC_PER_SEC)
		return -EINVAL;

	if (sec == 0 && nsec == 0)
		return 0;

	time_timeout_t timeout;
	int r = time_timeout_after_timespec(sec, nsec, &timeout);

	if (r != 0)
		return r;

	while (!time_timeout_expired(&timeout)) {
		time_sleep_until_interrupt_or_timeout(&timeout);
		r = syscall_interrupted();
		if (r != 0)
			return r;
	}

	return 0;
}

/* -------------------------------------------------------------------------- */
/* Dispatch table                                                             */
/* -------------------------------------------------------------------------- */

static syscall_handler_t syscall_table[] = {
	[SYS_READ] = sys_read_handler,
	[SYS_WRITE] = sys_write_handler,
	[SYS_OPEN] = sys_open_handler,
	[SYS_CLOSE] = sys_close_handler,
	[SYS_STAT] = sys_stat_handler,
	[SYS_FSTAT] = sys_fstat_handler,
	[SYS_LSEEK] = sys_lseek_handler,
	[SYS_ACCESS] = sys_access_handler,
	[SYS_GETDENTS] = sys_getdents_handler,

	[SYS_EXIT] = sys_exit_handler,
	[SYS_FORK] = sys_fork_handler,
	[SYS_EXECVE] = sys_execve_handler,
	[SYS_WAITPID] = sys_waitpid_handler,
	[SYS_NSLEEP] = sys_nsleep_handler,

	[SYS_CHMOD] = sys_chmod_handler,
	[SYS_CHOWN] = sys_chown_handler,
	[SYS_MKDIR] = sys_mkdir_handler,
	[SYS_RMDIR] = sys_rmdir_handler,
	[SYS_UNLINK] = sys_unlink_handler,
	[SYS_RENAMEAT] = sys_renameat_handler,
	[SYS_CHROOT] = sys_chroot_handler,
	[SYS_MOUNT] = sys_mount_handler,
	[SYS_CHANGE_ROOT] = sys_change_root_handler,
	[SYS_CHDIR] = sys_chdir_handler,
	[SYS_GETCWD] = sys_getcwd_handler,

	[SYS_ARCH_PRCTL] = sys_arch_prctl_handler,
	[SYS_MMAP] = sys_mmap_handler,
	[SYS_MUNMAP] = sys_munmap_handler,
	[SYS_MPROTECT] = sys_mprotect_handler,
	[SYS_IOCTL] = sys_ioctl_handler,
	[SYS_FCNTL] = sys_fcntl_handler,
	[SYS_DUP2] = sys_dup2_handler,
	[SYS_PIPE] = sys_pipe_handler,
	[SYS_UNAME] = sys_uname_handler,

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
	[SYS_PSELECT] = sys_pselect_handler,

	[SYS_GETPID] = sys_getpid_handler,
	[SYS_GETPPID] = sys_getppid_handler,
	[SYS_GETTID] = sys_gettid_handler,
	[SYS_GETUID] = sys_getuid_handler,
	[SYS_GETEUID] = sys_geteuid_handler,
	[SYS_GETGID] = sys_getgid_handler,
	[SYS_GETEGID] = sys_getegid_handler,
	[SYS_GETRESUID] = sys_getresuid_handler,
	[SYS_GETRESGID] = sys_getresgid_handler,
	[SYS_GETGROUPS] = sys_getgroups_handler,
	[SYS_SETUID] = sys_setuid_handler,
	[SYS_SETEUID] = sys_seteuid_handler,
	[SYS_SETGID] = sys_setgid_handler,
	[SYS_SETEGID] = sys_setegid_handler,
	[SYS_GETPGID] = sys_getpgid_handler,
	[SYS_SETPGID] = sys_setpgid_handler,
	[SYS_SETSID] = sys_setsid_handler,
	[SYS_KILL] = sys_kill_handler,
	[SYS_SIGACTION] = sys_sigaction_handler,
	[SYS_SIGPROCMASK] = sys_sigprocmask_handler,
	[SYS_SIGRETURN] = sys_sigreturn_handler,
};

interrupt_frame_t *syscall_dispatch(interrupt_frame_t *frame)
{
	if (!frame)
		return frame;

	uint64_t nr = frame->rax;

	if (nr < (sizeof(syscall_table) / sizeof(syscall_table[0])) &&
		syscall_table[nr]) {
		long ret = syscall_table[nr](frame);

		if (nr == SYS_EXIT || nr == SYS_CHANGE_ROOT || nr == SYS_SIGRETURN)
			return sched_signal_deliver((interrupt_frame_t *)(uintptr_t)ret);

		frame->rax = (uint64_t)(int64_t)ret;
		return sched_signal_deliver(frame);
	}

	tcb_t *thread = sched_current();

	log_warn("syscall", "unhandled syscall rax=%llu from tid=%d", nr,
			 thread ? thread->tid : -1);

	frame->rax = (uint64_t)(int64_t)-ENOSYS;
	return frame;
}