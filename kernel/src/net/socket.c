#include "internal.h"
#include <debug/log.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/vmm.h>
#include <net/net.h>
#include <net/socket.h>
#include <sched/sched.h>
#include <sync/spinlock.h>

#define SOCKET_BACKLOG 16
#define SOCKET_NAME_MAX 64
#define SOCKET_BUF_SIZE 4096

typedef struct socket_entry {
	socket_t *socket;
	struct socket_entry *next;
} socket_entry_t;

typedef struct unix_sock {
	char name[SOCKET_NAME_MAX + 1];
	socket_t *peer;
	char *rx_buf;
	size_t rx_cap;
	size_t rx_len;
	size_t rx_head;
	char *tx_buf;
	size_t tx_cap;
	size_t tx_len;
	volatile int readable;
	volatile int writable;
} unix_sock_t;

typedef struct inet_sock {
	net_tcp_conn_t *tcp_conn;
	uint16_t local_port;
	uint32_t remote_ip;
	uint16_t remote_port;
	char rx_buf[SOCKET_BUF_SIZE];
	size_t rx_len;
	size_t rx_head;
	volatile int readable;
} inet_sock_t;

struct socket {
	int domain;
	int type;
	int protocol;
	uint32_t flags;
	unix_sock_t *unix_data;
	inet_sock_t *inet_data;
	socket_t *backlog[SOCKET_BACKLOG];
	int backlog_count;
	int refcount;
};

static spinlock_t socket_lock = SPINLOCK_INIT;
static socket_entry_t *socket_list = NULL;
static int socket_count = 0;

static int socket_add(socket_t *sock);

int net_socket(socket_t **out, int domain, int type, int protocol)
{
	if (domain != AF_UNIX && domain != AF_INET)
		return NET_SOCK_ERR_INVAL;

	if (domain == AF_INET && type != SOCK_STREAM)
		return NET_SOCK_ERR_INVAL;

	socket_t *sock = kzalloc(sizeof(*sock));
	if (!sock)
		return NET_SOCK_ERR_NOMEM;

	sock->domain = domain;
	sock->type = type;
	sock->protocol = protocol;
	sock->refcount = 1;

	if (domain == AF_UNIX) {
		sock->unix_data = kzalloc(sizeof(*sock->unix_data));
		if (!sock->unix_data) {
			kfree(sock);
			return NET_SOCK_ERR_NOMEM;
		}
		sock->unix_data->rx_cap = SOCKET_BUF_SIZE;
		sock->unix_data->rx_buf = kzalloc(sock->unix_data->rx_cap);
		if (!sock->unix_data->rx_buf) {
			kfree(sock->unix_data);
			kfree(sock);
			return NET_SOCK_ERR_NOMEM;
		}
		sock->unix_data->tx_cap = SOCKET_BUF_SIZE;
		sock->unix_data->tx_buf = kzalloc(sock->unix_data->tx_cap);
		if (!sock->unix_data->tx_buf) {
			kfree(sock->unix_data->rx_buf);
			kfree(sock->unix_data);
			kfree(sock);
			return NET_SOCK_ERR_NOMEM;
		}
		sock->unix_data->readable = 0;
		sock->unix_data->writable = 1;
	} else if (domain == AF_INET) {
		sock->inet_data = kzalloc(sizeof(*sock->inet_data));
		if (!sock->inet_data) {
			kfree(sock);
			return NET_SOCK_ERR_NOMEM;
		}
		sock->inet_data->tcp_conn = NULL;
		sock->inet_data->local_port = 0;
		sock->inet_data->remote_ip = 0;
		sock->inet_data->remote_port = 0;
		sock->inet_data->rx_len = 0;
		sock->inet_data->rx_head = 0;
		sock->inet_data->readable = 0;
	}

	*out = sock;
	socket_add(sock);
	return NET_SOCK_OK;
}

static int socket_add(socket_t *sock)
{
	socket_entry_t *entry = kzalloc(sizeof(*entry));
	if (!entry)
		return NET_SOCK_ERR_NOMEM;

	entry->socket = sock;
	entry->next = socket_list;
	socket_list = entry;
	socket_count++;
	return NET_SOCK_OK;
}

static void socket_remove(socket_t *sock)
{
	socket_entry_t **cur = &socket_list;
	while (*cur) {
		if ((*cur)->socket == sock) {
			socket_entry_t *entry = *cur;
			*cur = entry->next;
			kfree(entry);
			socket_count--;
			return;
		}
		cur = &(*cur)->next;
	}
}

int net_bind(socket_t *sock, const sockaddr_t *addr, socklen_t addrlen)
{
	if (!sock || !addr)
		return NET_SOCK_ERR_INVAL;

	if (sock->domain == AF_UNIX) {
		if (addrlen < sizeof(sa_family_t_16))
			return NET_SOCK_ERR_INVAL;

		if (addr->sun.sun_family != AF_UNIX)
			return NET_SOCK_ERR_INVAL;

		size_t path_len = 0;
		while (path_len < addrlen - offsetof(sockaddr_un_t, sun_path) &&
			   path_len < sizeof(sock->unix_data->name) &&
			   addr->sun.sun_path[path_len]) {
			sock->unix_data->name[path_len] = addr->sun.sun_path[path_len];
			path_len++;
		}
		sock->unix_data->name[path_len] = '\0';

		sock->flags |= NET_SOCK_BINDED;
		log_debug("socket", "AF_UNIX bound to %s", sock->unix_data->name);
	} else if (sock->domain == AF_INET) {
		if (addrlen < sizeof(sockaddr_in_t))
			return NET_SOCK_ERR_INVAL;

		if (addr->sin.sin_family != AF_INET)
			return NET_SOCK_ERR_INVAL;

		sock->inet_data->local_port = ntohs(addr->sin.sin_port);
		sock->flags |= NET_SOCK_BINDED;
		log_debug("socket", "AF_INET bound to port %d",
				  sock->inet_data->local_port);
	}

	return NET_SOCK_OK;
}

int net_listen(socket_t *sock, int backlog)
{
	if (!sock)
		return NET_SOCK_ERR_INVAL;

	if (sock->domain == AF_UNIX) {
		sock->flags |= NET_SOCK_LISTENING;
		if (backlog > SOCKET_BACKLOG)
			backlog = SOCKET_BACKLOG;
		log_debug("socket", "AF_UNIX listening (backlog=%d)", backlog);
	} else if (sock->domain == AF_INET) {
		sock->flags |= NET_SOCK_LISTENING;
		log_debug("socket", "AF_INET listening (backlog=%d)", backlog);
	}

	return NET_SOCK_OK;
}

int net_accept(socket_t *sock, socket_t **out, sockaddr_t *addr,
			   socklen_t *addrlen)
{
	if (!sock || !out)
		return NET_SOCK_ERR_INVAL;

	if (sock->domain == AF_UNIX) {
		if (!(sock->flags & NET_SOCK_LISTENING))
			return NET_SOCK_ERR_INVAL;

		if (sock->backlog_count == 0) {
			return NET_SOCK_ERR_INVAL;
		}

		socket_t *client = sock->backlog[0];
		for (int i = 0; i < sock->backlog_count - 1; i++) {
			sock->backlog[i] = sock->backlog[i + 1];
		}
		sock->backlog_count--;

		*out = client;

		if (addr && addrlen && *addrlen >= sizeof(sa_family_t_16)) {
			memset(addr, 0, sizeof(*addr));
			addr->sun.sun_family = AF_UNIX;
			*addrlen = sizeof(sa_family_t_16);
		}

		log_debug("socket", "accepted connection");
		return NET_SOCK_OK;
	}

	if (sock->domain == AF_INET) {
		if (sock->backlog_count == 0) {
			return NET_SOCK_ERR_WOULDBLOCK;
		}

		socket_t *client = sock->backlog[0];

		for (int i = 0; i < sock->backlog_count - 1; i++)
			sock->backlog[i] = sock->backlog[i + 1];

		sock->backlog_count--;

		*out = client;

		if (addr && addrlen && *addrlen >= sizeof(sockaddr_in_t)) {
			memset(addr, 0, sizeof(*addr));
			addr->sin.sin_family = AF_INET;
			addr->sin.sin_addr = htonl(client->inet_data->remote_ip);
			addr->sin.sin_port = htons(client->inet_data->remote_port);
			*addrlen = sizeof(sockaddr_in_t);
		}

		return NET_SOCK_OK;
	}

	return NET_SOCK_ERR_INVAL;
}

int net_connect(socket_t *sock, const sockaddr_t *addr, socklen_t addrlen)
{
	if (!sock || !addr)
		return NET_SOCK_ERR_INVAL;

	if (sock->domain == AF_UNIX) {
		if (addr->sun.sun_family != AF_UNIX)
			return NET_SOCK_ERR_INVAL;

		socket_entry_t *entry;
		for (entry = socket_list; entry; entry = entry->next) {
			socket_t *ls = entry->socket;
			if (ls->domain == AF_UNIX && ls->unix_data &&
				(ls->flags & NET_SOCK_LISTENING)) {
				size_t name_len = 0;
				char name[SOCKET_NAME_MAX + 1] = { 0 };
				size_t path_len = addrlen - offsetof(sockaddr_un_t, sun_path);
				if (path_len > SOCKET_NAME_MAX)
					path_len = SOCKET_NAME_MAX;

				for (size_t i = 0; i < path_len && addr->sun.sun_path[i]; i++) {
					name[i] = addr->sun.sun_path[i];
					name_len++;
				}
				name[name_len] = '\0';

				if (strncmp(ls->unix_data->name, name, SOCKET_NAME_MAX) == 0) {
					socket_t *client = NULL;
					int r = net_socket(&client, AF_UNIX, SOCK_STREAM, 0);
					if (r != NET_SOCK_OK)
						return r;

					client->unix_data->peer = ls;
					client->flags |= NET_SOCK_CONNECTED | NET_SOCK_BINDED;

					if (ls->backlog_count < SOCKET_BACKLOG) {
						ls->backlog[ls->backlog_count++] = client;
						sock->unix_data->peer = ls;
						sock->flags |= NET_SOCK_CONNECTED | NET_SOCK_BINDED;

						log_debug("socket", "connected to %s", name);
						return NET_SOCK_OK;
					}

					net_close(client);
					return NET_SOCK_ERR_INVAL;
				}
			}
		}

		return NET_SOCK_ERR_NOENT;
	} else if (sock->domain == AF_INET) {
		if (addrlen < sizeof(sockaddr_in_t))
			return NET_SOCK_ERR_INVAL;

		if (addr->sin.sin_family != AF_INET)
			return NET_SOCK_ERR_INVAL;

		sock->inet_data->remote_ip = ntohl(addr->sin.sin_addr);
		sock->inet_data->remote_port = ntohs(addr->sin.sin_port);

		/* Use net_route so loopback (127.x) goes via lo, not eth0. */
		uint32_t next_hop = 0;
		netdev_t *dev = net_route(sock->inet_data->remote_ip, &next_hop);
		if (!dev)
			return NET_SOCK_ERR_NOENT;

		net_tcp_conn_t *conn = NULL;
		int r = net_tcp_connect_ip(dev, sock->inet_data->remote_ip,
								   sock->inet_data->remote_port, &conn, 5000);

		if (r != VFS_OK) {
			log_debug("socket", "AF_INET connect failed r=%d", r);
			return NET_SOCK_ERR_NOENT;
		}

		sock->inet_data->tcp_conn = conn;
		sock->flags |= NET_SOCK_BINDED | NET_SOCK_CONNECTED;

		log_debug("socket", "AF_INET connected to %u.%u.%u.%u:%d",
				  (sock->inet_data->remote_ip >> 24) & 0xff,
				  (sock->inet_data->remote_ip >> 16) & 0xff,
				  (sock->inet_data->remote_ip >> 8) & 0xff,
				  sock->inet_data->remote_ip & 0xff,
				  sock->inet_data->remote_port);
		return NET_SOCK_OK;
	}

	return NET_SOCK_ERR_INVAL;
}

int net_send(socket_t *sock, const void *buf, size_t len, int flags)
{
	return net_sendto(sock, buf, len, flags, NULL, 0);
}

int net_sendto(socket_t *sock, const void *buf, size_t len, int flags,
			   const sockaddr_t *dest, socklen_t addrlen)
{
	(void)dest;
	(void)addrlen;
	(void)flags;

	if (!sock || !buf)
		return NET_SOCK_ERR_INVAL;

	if (sock->domain == AF_UNIX) {
		unix_sock_t *us = sock->unix_data;
		if (!us || !us->peer)
			return NET_SOCK_ERR_NOTCONN;

		if (!(sock->flags & NET_SOCK_CONNECTED))
			return NET_SOCK_ERR_NOTCONN;

		unix_sock_t *peer = us->peer->unix_data;
		if (!peer)
			return NET_SOCK_ERR_NOTCONN;

		size_t to_copy = len;
		if (to_copy > peer->rx_cap - peer->rx_len)
			to_copy = peer->rx_cap - peer->rx_len;

		if (to_copy == 0)
			return NET_SOCK_ERR_INVAL;

		for (size_t i = 0; i < to_copy; i++) {
			peer->rx_buf[(peer->rx_head + peer->rx_len) % peer->rx_cap] =
				((const char *)buf)[i];
			peer->rx_len++;
		}

		peer->readable = 1;

		return (int)to_copy;
	} else if (sock->domain == AF_INET) {
		inet_sock_t *is = sock->inet_data;
		if (!is->tcp_conn)
			return NET_SOCK_ERR_NOTCONN;

		if (!(sock->flags & NET_SOCK_CONNECTED))
			return NET_SOCK_ERR_WOULDBLOCK;

		size_t done = 0;
		int r = net_tcp_send(is->tcp_conn, buf, len, &done, 5000);
		if (r != VFS_OK)
			return NET_SOCK_ERR_INVAL;

		return (int)done;
	}

	return NET_SOCK_ERR_INVAL;
}

int net_recv(socket_t *sock, void *buf, size_t len, int flags)
{
	return net_recvfrom(sock, buf, len, flags, NULL, NULL);
}

int net_recvfrom(socket_t *sock, void *buf, size_t len, int flags,
				 sockaddr_t *addr, socklen_t *addrlen)
{
	(void)flags;

	if (!sock || !buf)
		return NET_SOCK_ERR_INVAL;

	if (sock->domain == AF_UNIX) {
		unix_sock_t *us = sock->unix_data;
		if (!us || !us->peer)
			return NET_SOCK_ERR_NOTCONN;

		if (!(sock->flags & NET_SOCK_CONNECTED))
			return NET_SOCK_ERR_NOTCONN;

		size_t to_read = us->rx_len;
		if (to_read > len)
			to_read = len;

		if (to_read == 0)
			return 0;

		for (size_t i = 0; i < to_read; i++) {
			((char *)buf)[i] = us->rx_buf[us->rx_head];
			us->rx_head = (us->rx_head + 1) % us->rx_cap;
			us->rx_len--;
		}

		if (us->rx_len == 0)
			us->readable = 0;

		return (int)to_read;
	} else if (sock->domain == AF_INET) {
		if (!sock->inet_data->tcp_conn)
			return NET_SOCK_ERR_NOTCONN;

		if (!(sock->flags & NET_SOCK_CONNECTED))
			return NET_SOCK_ERR_WOULDBLOCK;

		if (sock->inet_data->rx_len == 0) {
			size_t done = 0;
			int r =
				net_tcp_recv(sock->inet_data->tcp_conn, sock->inet_data->rx_buf,
							 SOCKET_BUF_SIZE, &done, 10000);

			if (r == VFS_ERR_TIMEOUT)
				return NET_SOCK_ERR_WOULDBLOCK;

			if (r != VFS_OK)
				return NET_SOCK_ERR_NOTCONN;

			/*
			 * done == 0 with VFS_OK is TCP EOF. Return 0 to userspace
			 * instead of translating EOF into ENOTCONN.
			 */
			if (done == 0)
				return 0;

			sock->inet_data->rx_len = done;
			sock->inet_data->rx_head = 0;
		}

		size_t to_read = sock->inet_data->rx_len;
		if (to_read > len)
			to_read = len;

		if (to_read == 0)
			return 0;

		memcpy(buf, sock->inet_data->rx_buf + sock->inet_data->rx_head,
			   to_read);
		sock->inet_data->rx_head += to_read;
		sock->inet_data->rx_len -= to_read;

		return (int)to_read;
	}

	return NET_SOCK_ERR_INVAL;
}

int net_shutdown(socket_t *sock, int how)
{
	if (!sock)
		return NET_SOCK_ERR_INVAL;

	log_debug("socket", "shutdown how=%d", how);

	if (sock->domain == AF_INET && sock->inet_data &&
		sock->inet_data->tcp_conn) {
		net_tcp_close(sock->inet_data->tcp_conn);
		sock->inet_data->tcp_conn = NULL;
	}

	return NET_SOCK_OK;
}

int net_close(socket_t *sock)
{
	if (!sock)
		return NET_SOCK_ERR_INVAL;

	sock->refcount--;

	if (sock->refcount > 0)
		return NET_SOCK_OK;

	if (sock->unix_data) {
		if (sock->unix_data->rx_buf)
			kfree(sock->unix_data->rx_buf);
		if (sock->unix_data->tx_buf)
			kfree(sock->unix_data->tx_buf);
		kfree(sock->unix_data);
	}

	if (sock->inet_data) {
		if (sock->inet_data->tcp_conn)
			net_tcp_close(sock->inet_data->tcp_conn);
		kfree(sock->inet_data);
	}

	socket_remove(sock);
	kfree(sock);

	return NET_SOCK_OK;
}

int net_getsockname(socket_t *sock, sockaddr_t *addr, socklen_t *addrlen)
{
	if (!sock || !addr || !addrlen)
		return NET_SOCK_ERR_INVAL;

	if (sock->domain == AF_UNIX && sock->unix_data) {
		memset(addr, 0, sizeof(*addr));
		addr->sun.sun_family = AF_UNIX;
		size_t len = 0;
		while (len < SOCKET_NAME_MAX && sock->unix_data->name[len])
			len++;
		if (len + 1 > *addrlen)
			len = *addrlen - 1;
		if (len > 0)
			memcpy(addr->sun.sun_path, sock->unix_data->name, len);
		*addrlen = sizeof(sa_family_t_16) + len;
	} else if (sock->domain == AF_INET) {
		memset(addr, 0, sizeof(*addr));
		addr->sin.sin_family = AF_INET;
		addr->sin.sin_addr =
			sock->inet_data ? htonl(sock->inet_data->remote_ip) : 0;
		addr->sin.sin_port =
			sock->inet_data ? htons(sock->inet_data->remote_port) : 0;
		*addrlen = sizeof(sockaddr_in_t);
	}

	return NET_SOCK_OK;
}

int net_getpeername(socket_t *sock, sockaddr_t *addr, socklen_t *addrlen)
{
	return net_getsockname(sock, addr, addrlen);
}

int net_setsockopt(socket_t *sock, int level, int optname, const void *optval,
				   socklen_t optlen)
{
	(void)sock;
	(void)level;
	(void)optname;
	(void)optval;
	(void)optlen;
	return NET_SOCK_OK;
}

int net_getsockopt(socket_t *sock, int level, int optname, void *optval,
				   socklen_t *optlen)
{
	if (!sock || !optval || !optlen)
		return NET_SOCK_ERR_INVAL;

	if (level == SOL_SOCKET && optname == SO_TYPE) {
		if (*optlen < sizeof(int))
			return NET_SOCK_ERR_INVAL;
		*(int *)optval = sock->type;
		*optlen = sizeof(int);
		return NET_SOCK_OK;
	}

	if (level == SOL_SOCKET && optname == SO_ERROR) {
		if (*optlen < sizeof(int))
			return NET_SOCK_ERR_INVAL;
		*(int *)optval = 0;
		*optlen = sizeof(int);
		return NET_SOCK_OK;
	}

	return NET_SOCK_ERR_INVAL;
}

int socket_init(void)
{
	log_info("socket", "initializing socket layer");
	return NET_SOCK_OK;
}