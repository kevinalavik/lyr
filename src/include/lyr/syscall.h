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

static inline long lyr_chdir(const char *path)
{
	return lyr_syscall3(SYS_CHDIR, (long)path, 0, 0);
}

static inline long lyr_getcwd(char *buf, size_t size)
{
	return lyr_syscall3(SYS_GETCWD, (long)buf, (long)size, 0);
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

#define AF_UNIX 1
#define AF_INET 2

#define SOCK_STREAM 1
#define SOCK_DGRAM 2

typedef unsigned short sa_family_t;
typedef unsigned int socklen_t;

static inline unsigned short htons(unsigned short h)
{
	return ((h & 0xff) << 8) | ((h >> 8) & 0xff);
}

static inline unsigned int htonl(unsigned int h)
{
	return ((h & 0xff) << 24) | ((h & 0xff00) << 8) |
		   ((h >> 8) & 0xff00) | ((h >> 24) & 0xff);
}

static inline unsigned int ntohl(unsigned int n)
{
	return htonl(n);
}

static inline unsigned short ntohs(unsigned short n)
{
	return htons(n);
}

typedef struct {
	char sun_path[108];
	sa_family_t sun_family;
} sockaddr_un_t;

typedef struct {
	sa_family_t sin_family;
	unsigned short sin_port;
	unsigned int sin_addr;
	unsigned char sin_zero[8];
} sockaddr_in_t;

typedef union {
	sa_family_t sa_family;
	sockaddr_un_t sun;
	sockaddr_in_t sin;
} sockaddr_t;

static inline int lyr_socket(int domain, int type, int protocol)
{
	return (int)lyr_syscall3(SYS_SOCKET, domain, type, protocol);
}

static inline int lyr_bind(int fd, const sockaddr_t *addr, socklen_t addrlen)
{
	return (int)lyr_syscall3(SYS_BIND, fd, (long)addr, addrlen);
}

static inline int lyr_connect(int fd, const sockaddr_t *addr, socklen_t addrlen)
{
	return (int)lyr_syscall3(SYS_CONNECT, fd, (long)addr, addrlen);
}

static inline int lyr_listen(int fd, int backlog)
{
	return (int)lyr_syscall3(SYS_LISTEN, fd, backlog, 0);
}

static inline int lyr_accept(int fd, sockaddr_t *addr, socklen_t *addrlen)
{
	return (int)lyr_syscall3(SYS_ACCEPT, fd, (long)addr, (long)addrlen);
}

static inline long lyr_send(int fd, const void *buf, size_t len, int flags)
{
	return lyr_syscall3(SYS_SEND, fd, (long)buf, (long)len);
}

static inline long lyr_recv(int fd, void *buf, size_t len, int flags)
{
	return lyr_syscall3(SYS_RECV, fd, (long)buf, (long)len);
}

static inline long lyr_sendto(int fd, const void *buf, size_t len, int flags,
							   const sockaddr_t *dest, socklen_t dest_len)
{
	return lyr_syscall6(SYS_SENDTO, fd, (long)buf, (long)len, flags,
						(long)dest, dest_len);
}

static inline long lyr_recvfrom(int fd, void *buf, size_t len, int flags,
								 sockaddr_t *addr, socklen_t *addrlen)
{
	return lyr_syscall6(SYS_RECVFROM, fd, (long)buf, (long)len, flags,
						(long)addr, (long)addrlen);
}

static inline int lyr_shutdown(int fd, int how)
{
	return (int)lyr_syscall3(SYS_SHUTDOWN, fd, how, 0);
}

static inline int lyr_getsockname(int fd, sockaddr_t *addr, socklen_t *addrlen)
{
	return (int)lyr_syscall3(SYS_GETSOCKNAME, fd, (long)addr, (long)addrlen);
}

static inline int lyr_getpeername(int fd, sockaddr_t *addr, socklen_t *addrlen)
{
	return (int)lyr_syscall3(SYS_GETPEERNAME, fd, (long)addr, (long)addrlen);
}

static inline void lyr_exit(int status)
{
	lyr_syscall3(SYS_EXIT, status, 0, 0);
	for (;;)
		__asm__ volatile("hlt");
}

#endif /* _LYR_USER_SYSCALL_H */
