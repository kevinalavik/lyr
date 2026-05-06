#ifndef _LYR_NET_SOCKET_H
#define _LYR_NET_SOCKET_H

#include <stddef.h>
#include <stdint.h>

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

#define NET_SOCKET_ERR_BASE -100

#define NET_SOCK_OK 0
#define NET_SOCK_ERR_INVAL (NET_SOCKET_ERR_BASE - 1)
#define NET_SOCK_ERR_NOMEM (NET_SOCKET_ERR_BASE - 2)
#define NET_SOCK_ERR_BADF (NET_SOCKET_ERR_BASE - 3)
#define NET_SOCK_ERR_NOTCONN (NET_SOCKET_ERR_BASE - 4)
#define NET_SOCK_ERR_INPROGRESS (NET_SOCKET_ERR_BASE - 5)
#define NET_SOCK_ERR_ALREADY (NET_SOCKET_ERR_BASE - 6)
#define NET_SOCK_ERR_ISCONN (NET_SOCKET_ERR_BASE - 7)
#define NET_SOCK_ERR_MSGSIZE (NET_SOCKET_ERR_BASE - 8)
#define NET_SOCK_ERR_WOULDBLOCK (NET_SOCKET_ERR_BASE - 9)
#define NET_SOCK_ERR_NOENT (NET_SOCKET_ERR_BASE - 10)

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
int net_close(socket_t *sock);
int net_getsockname(socket_t *sock, sockaddr_t *addr, socklen_t *addrlen);
int net_getpeername(socket_t *sock, sockaddr_t *addr, socklen_t *addrlen);
int net_setsockopt(socket_t *sock, int level, int optname, const void *optval,
				   socklen_t optlen);
int net_getsockopt(socket_t *sock, int level, int optname, void *optval,
				   socklen_t *optlen);

int socket_create_vfs_node(socket_t *sock, const char *path);

#endif