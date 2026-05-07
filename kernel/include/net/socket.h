#ifndef _LYR_NET_SOCKET_H
#define _LYR_NET_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <errno.h>

#define AF_UNIX 1
#define AF_INET 2
#define AF_INET6 10

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_SEQPACKET 5

#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x80000

#define SOL_SOCKET 1

#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_SNDBUF 5
#define SO_RCVBUF 6
#define SO_PRIORITY 7

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

#define MSG_PEEK 0x2
#define MSG_DONTWAIT 0x40

#define NET_SOCK_OK 0

/*
 * Socket errors are returned directly through the syscall ABI, so keep them
 * equal to negative Linux errno values.  mlibc expects negative Linux errno
 * numbers from syscalls and translates them into userspace errno/h_errno.
 */
#define NET_SOCK_ERR_INVAL (-EINVAL)
#define NET_SOCK_ERR_NOMEM (-ENOMEM)
#define NET_SOCK_ERR_BADF (-EBADF)
#define NET_SOCK_ERR_NOTCONN (-ENOTCONN)
#define NET_SOCK_ERR_INPROGRESS (-EINPROGRESS)
#define NET_SOCK_ERR_ALREADY (-EALREADY)
#define NET_SOCK_ERR_ISCONN (-EISCONN)
#define NET_SOCK_ERR_MSGSIZE (-EMSGSIZE)
#define NET_SOCK_ERR_WOULDBLOCK (-EAGAIN)
#define NET_SOCK_ERR_NOENT (-ENOENT)
#define NET_SOCK_ERR_AFNOSUPPORT (-EAFNOSUPPORT)
#define NET_SOCK_ERR_PROTONOSUPPORT (-EPROTONOSUPPORT)
#define NET_SOCK_ERR_ADDRINUSE (-EADDRINUSE)
#define NET_SOCK_ERR_ADDRNOTAVAIL (-EADDRNOTAVAIL)
#define NET_SOCK_ERR_NETUNREACH (-ENETUNREACH)
#define NET_SOCK_ERR_CONNRESET (-ECONNRESET)
#define NET_SOCK_ERR_CONNREFUSED (-ECONNREFUSED)
#define NET_SOCK_ERR_TIMEDOUT (-ETIMEDOUT)

#define NET_SOCK_NONBLOCK 0x01
#define NET_SOCK_LISTENING 0x02
#define NET_SOCK_CONNECTED 0x04
#define NET_SOCK_BINDED 0x08

typedef struct socket socket_t;

typedef int8_t sa_family_t;
typedef uint32_t socklen_t;
typedef uint16_t sa_family_t_16;

typedef struct {
	int domain;
	int type;
	int protocol;
} socket_attr_t;

typedef struct {
	sa_family_t_16 sun_family;
	char sun_path[108];
} sockaddr_un_t;

typedef struct {
	sa_family_t_16 sin_family;
	uint16_t sin_port;
	uint32_t sin_addr;
	uint8_t sin_zero[8];
} sockaddr_in_t;

typedef union {
	sa_family_t_16 sa_family;
	sockaddr_un_t sun;
	sockaddr_in_t sin;
} sockaddr_t;

int socket_init(void);
int net_socket(socket_t **out, int domain, int type, int protocol);
int net_bind(socket_t *sock, const sockaddr_t *addr, socklen_t addrlen);
int net_listen(socket_t *sock, int backlog);
int net_accept(socket_t *sock, socket_t **out, sockaddr_t *addr,
			   socklen_t *addrlen);
int net_connect(socket_t *sock, const sockaddr_t *addr, socklen_t addrlen);
int net_send(socket_t *sock, const void *buf, size_t len, int flags);
int net_recv(socket_t *sock, void *buf, size_t len, int flags);
int net_sendto(socket_t *sock, const void *buf, size_t len, int flags,
			   const sockaddr_t *dest, socklen_t addrlen);
int net_recvfrom(socket_t *sock, void *buf, size_t len, int flags,
				 sockaddr_t *addr, socklen_t *addrlen);
int net_shutdown(socket_t *sock, int how);
int net_socket_ref(socket_t *sock);
int net_close(socket_t *sock);
int net_getsockname(socket_t *sock, sockaddr_t *addr, socklen_t *addrlen);
int net_getpeername(socket_t *sock, sockaddr_t *addr, socklen_t *addrlen);
int net_setsockopt(socket_t *sock, int level, int optname, const void *optval,
				   socklen_t optlen);
int net_getsockopt(socket_t *sock, int level, int optname, void *optval,
				   socklen_t *optlen);
int net_poll_socket(socket_t *sock, int events);

int socket_create_vfs_node(socket_t *sock, const char *path);

#endif