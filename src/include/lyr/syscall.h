#ifndef _LYR_USER_SYSCALL_H
#define _LYR_USER_SYSCALL_H

#include <stddef.h>
#include <syscall.h>
#include <lyr/vfs.h>
#include <lyr/syscall_asm.h>

static inline long lyr_read(int fd, void *buf, size_t len)
{
	return lyr_syscall3(SYS_READ, fd, (long)buf, (long)len);
}

static inline long lyr_write(int fd, const void *buf, size_t len)
{
	return lyr_syscall3(SYS_WRITE, fd, (long)buf, (long)len);
}

static inline long lyr_open(const char *path, long flags, long mode)
{
	return lyr_syscall3(SYS_OPEN, (long)path, flags, mode);
}

static inline long lyr_close(int fd)
{
	return lyr_syscall3(SYS_CLOSE, fd, 0, 0);
}

static inline long lyr_lseek(int fd, long off, long whence)
{
	return lyr_syscall3(SYS_LSEEK, fd, off, whence);
}

static inline long lyr_stat(const char *path, lyr_stat_t *st)
{
	return lyr_syscall3(SYS_STAT, (long)path, (long)st, 0);
}

static inline long lyr_access(const char *path, int mode)
{
	return lyr_syscall3(SYS_ACCESS, (long)path, mode, 0);
}

static inline long lyr_chmod(const char *path, long mode)
{
	return lyr_syscall3(SYS_CHMOD, (long)path, mode, 0);
}

static inline long lyr_chown(const char *path, long uid, long gid)
{
	return lyr_syscall3(SYS_CHOWN, (long)path, uid, gid);
}

static inline long lyr_getdents(int fd, lyr_dirent_t *ents, size_t cap)
{
	return lyr_syscall3(SYS_GETDENTS, fd, (long)ents, (long)cap);
}

static inline long lyr_mkdir(const char *path, long mode)
{
	return lyr_syscall3(SYS_MKDIR, (long)path, mode, 0);
}

static inline long lyr_rmdir(const char *path)
{
	return lyr_syscall3(SYS_RMDIR, (long)path, 0, 0);
}

static inline long lyr_unlink(const char *path)
{
	return lyr_syscall3(SYS_UNLINK, (long)path, 0, 0);
}

static inline long lyr_chroot(const char *path)
{
	return lyr_syscall3(SYS_CHROOT, (long)path, 0, 0);
}

static inline long lyr_mount(const char *source, const char *target,
							 const char *fstype, long flags, const void *data)
{
	return lyr_syscall5(SYS_MOUNT, (long)source, (long)target, (long)fstype,
						flags, (long)data);
}


static inline long lyr_change_root(const char *source, const char *fstype,
								   const char *init_path)
{
	return lyr_syscall3(SYS_CHANGE_ROOT, (long)source, (long)fstype,
						 (long)init_path);
}

static inline long lyr_execve(const char *path, char *const argv[],
							  char *const envp[])
{
	return lyr_syscall3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
}

static inline long lyr_arch_prctl(long code, unsigned long addr)
{
	return lyr_syscall3(SYS_ARCH_PRCTL, code, addr, 0);
}

static inline void lyr_exit(int status)
{
	lyr_syscall3(SYS_EXIT, status, 0, 0);
	for (;;)
		__asm__ volatile("hlt");
}

#endif /* _LYR_USER_SYSCALL_H */
