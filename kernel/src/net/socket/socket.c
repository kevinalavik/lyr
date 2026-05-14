#include "../stack.h"
#include <dev/time.h>
#include <dev/pit.h>
#include <debug/log.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/vmm.h>
#include <net/net.h>
#include <net/socket.h>
#include <sys/poll.h>
#include <sched/sched.h>
#include <sync/spinlock.h>

#define SOCKET_BACKLOG 16
#define SOCKET_NAME_MAX 64
#define SOCKET_BUF_SIZE 4096
#define UDP_EPHEMERAL_MIN 49152
#define UDP_EPHEMERAL_MAX 65535

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
	uint32_t local_ip;
	uint16_t local_port;
	uint32_t remote_ip;
	uint16_t remote_port;
	uint32_t packet_src_ip;
	uint16_t packet_src_port;
	char rx_buf[SOCKET_BUF_SIZE];
	size_t rx_len;
	size_t rx_head;
	volatile int readable;
	uint64_t recv_timeout_ms;
	int ttl;
} inet_sock_t;

typedef struct socket_timeval {
	long tv_sec;
	long tv_usec;
} socket_timeval_t;

struct socket {
	int domain;
	int type;
	int protocol;
	uint32_t flags;
	const net_socket_domain_ops_t *domain_ops;
	void *private_data;
	unix_sock_t *unix_data;
	inet_sock_t *inet_data;
	socket_t *backlog[SOCKET_BACKLOG];
	int backlog_count;
	int refcount;
};

static spinlock_t socket_lock = SPINLOCK_INIT;
static socket_entry_t *socket_list = NULL;
static int socket_count = 0;
static uint16_t udp_next_ephemeral = UDP_EPHEMERAL_MIN;

#define SOCKET_DOMAIN_MAX 8

static const net_socket_domain_ops_t *socket_domains[SOCKET_DOMAIN_MAX];
static int socket_builtin_domains_ready;

static int socket_add(socket_t *sock);
static void socket_remove(socket_t *sock);
static int udp_auto_bind(socket_t *sock);
static int socket_udp_receive(const net_udp_dgram_t *dgram, void *ctx);
static int tcp_socket_accept(netdev_t *dev, uint32_t remote_ip,
							 uint16_t remote_port, net_tcp_conn_t *conn,
							 void *ctx);
static int unix_socket_validate(int type, int protocol);
static int unix_socket_init(socket_t *sock);
static void unix_socket_destroy(socket_t *sock);
static int inet_socket_validate(int type, int protocol);
static int inet_socket_init(socket_t *sock);
static void inet_socket_destroy(socket_t *sock);
static void socket_register_builtin_domains(void);
static const net_socket_domain_ops_t *socket_domain_lookup(int domain);

static const net_socket_domain_ops_t unix_socket_domain = {
	.domain = AF_UNIX,
	.name = "AF_UNIX",
	.validate = unix_socket_validate,
	.init = unix_socket_init,
	.destroy = unix_socket_destroy,
};

static const net_socket_domain_ops_t inet_socket_domain = {
	.domain = AF_INET,
	.name = "AF_INET",
	.validate = inet_socket_validate,
	.init = inet_socket_init,
	.destroy = inet_socket_destroy,
};

static int socket_interrupted(void)
{
	tcb_t *thread = sched_current();
	return (thread && sched_signal_is_pending(thread)) ? -EINTR : 0;
}

static const net_udp_handler_ops_t socket_udp_handler = {
	.name = "socket",
	.receive = socket_udp_receive,
	.ctx = NULL,
};

int net_socket_register_domain(const net_socket_domain_ops_t *ops)
{
	if (!ops || !ops->domain || !ops->name || !ops->validate || !ops->init ||
		!ops->destroy)
		return NET_SOCK_ERR_INVAL;

	for (size_t i = 0; i < SOCKET_DOMAIN_MAX; i++) {
		if (socket_domains[i] && socket_domains[i]->domain == ops->domain)
			return NET_SOCK_ERR_ADDRINUSE;
	}

	for (size_t i = 0; i < SOCKET_DOMAIN_MAX; i++) {
		if (!socket_domains[i]) {
			socket_domains[i] = ops;
			return NET_SOCK_OK;
		}
	}

	return NET_SOCK_ERR_NOMEM;
}

static void socket_register_builtin_domains(void)
{
	if (socket_builtin_domains_ready)
		return;

	(void)net_socket_register_domain(&unix_socket_domain);
	(void)net_socket_register_domain(&inet_socket_domain);
	socket_builtin_domains_ready = 1;
}

static const net_socket_domain_ops_t *socket_domain_lookup(int domain)
{
	socket_register_builtin_domains();

	for (size_t i = 0; i < SOCKET_DOMAIN_MAX; i++) {
		if (socket_domains[i] && socket_domains[i]->domain == domain)
			return socket_domains[i];
	}

	return NULL;
}

int net_socket(socket_t **out, int domain, int type, int protocol)
{
	int type_flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
	int base_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
	const net_socket_domain_ops_t *ops;
	int r;

	if (!out)
		return NET_SOCK_ERR_INVAL;

	*out = NULL;
	type = base_type;

	ops = socket_domain_lookup(domain);
	if (!ops)
		return NET_SOCK_ERR_AFNOSUPPORT;

	r = ops->validate(type, protocol);
	if (r != NET_SOCK_OK)
		return r;

	socket_t *sock = kzalloc(sizeof(*sock));
	if (!sock)
		return NET_SOCK_ERR_NOMEM;

	sock->domain = domain;
	sock->type = type;
	sock->protocol = protocol;
	sock->domain_ops = ops;
	if (type_flags & SOCK_NONBLOCK)
		sock->flags |= NET_SOCK_NONBLOCK;
	sock->refcount = 1;

	r = ops->init(sock);
	if (r != NET_SOCK_OK) {
		kfree(sock);
		return r;
	}

	r = socket_add(sock);
	if (r != NET_SOCK_OK) {
		ops->destroy(sock);
		kfree(sock);
		return r;
	}

	*out = sock;
	return NET_SOCK_OK;
}

static int unix_socket_validate(int type, int protocol)
{
	(void)protocol;

	if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET)
		return NET_SOCK_ERR_PROTONOSUPPORT;

	return NET_SOCK_OK;
}

static int unix_socket_init(socket_t *sock)
{
	if (!sock)
		return NET_SOCK_ERR_INVAL;

	sock->unix_data = kzalloc(sizeof(*sock->unix_data));
	if (!sock->unix_data)
		return NET_SOCK_ERR_NOMEM;

	sock->unix_data->rx_cap = SOCKET_BUF_SIZE;
	sock->unix_data->rx_buf = kzalloc(sock->unix_data->rx_cap);
	if (!sock->unix_data->rx_buf) {
		kfree(sock->unix_data);
		sock->unix_data = NULL;
		return NET_SOCK_ERR_NOMEM;
	}

	sock->unix_data->tx_cap = SOCKET_BUF_SIZE;
	sock->unix_data->tx_buf = kzalloc(sock->unix_data->tx_cap);
	if (!sock->unix_data->tx_buf) {
		kfree(sock->unix_data->rx_buf);
		kfree(sock->unix_data);
		sock->unix_data = NULL;
		return NET_SOCK_ERR_NOMEM;
	}

	sock->unix_data->readable = 0;
	sock->unix_data->writable = 1;
	return NET_SOCK_OK;
}

static void unix_socket_destroy(socket_t *sock)
{
	if (!sock || !sock->unix_data)
		return;

	if (sock->unix_data->rx_buf)
		kfree(sock->unix_data->rx_buf);
	if (sock->unix_data->tx_buf)
		kfree(sock->unix_data->tx_buf);
	kfree(sock->unix_data);
	sock->unix_data = NULL;
}

static int inet_socket_validate(int type, int protocol)
{
	if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW)
		return NET_SOCK_ERR_PROTONOSUPPORT;

	if (type == SOCK_RAW && protocol != IPPROTO_ICMP)
		return NET_SOCK_ERR_PROTONOSUPPORT;

	return NET_SOCK_OK;
}

static int inet_socket_init(socket_t *sock)
{
	if (!sock)
		return NET_SOCK_ERR_INVAL;

	sock->inet_data = kzalloc(sizeof(*sock->inet_data));
	if (!sock->inet_data)
		return NET_SOCK_ERR_NOMEM;

	sock->inet_data->recv_timeout_ms = 10000;
	sock->inet_data->ttl = 64;
	return NET_SOCK_OK;
}

static void inet_socket_destroy(socket_t *sock)
{
	if (!sock || !sock->inet_data)
		return;

	if (sock->inet_data->tcp_conn)
		net_tcp_close(sock->inet_data->tcp_conn);
	kfree(sock->inet_data);
	sock->inet_data = NULL;
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


static int ipv4_addr_is_local(uint32_t ip)
{
	if (ip == 0)
		return 1;

	for (netdev_t *dev = net_first_dev(); dev; dev = dev->next) {
		if (dev->ipv4_addr == ip)
			return 1;
	}

	return 0;
}

static int inet_bind_conflicts(socket_t *sock, uint32_t local_ip,
							  uint16_t port)
{
	if (!port)
		return 0;

	for (socket_entry_t *entry = socket_list; entry; entry = entry->next) {
		socket_t *s = entry->socket;
		if (!s || s == sock || s->domain != AF_INET || !s->inet_data)
			continue;

		if (s->inet_data->local_port != port)
			continue;

		/* Wildcard conflicts with every same-port bind. Exact binds conflict
		 * with the same exact address and with wildcard binds. */
		if (s->inet_data->local_ip == 0 || local_ip == 0 ||
			s->inet_data->local_ip == local_ip)
			return 1;
	}

	return 0;
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


static int tcp_socket_accept(netdev_t *dev, uint32_t remote_ip,
							 uint16_t remote_port, net_tcp_conn_t *conn,
							 void *ctx)
{
	socket_t *listener = ctx;
	if (!listener || !listener->inet_data || !conn)
		return NET_SOCK_ERR_INVAL;

	if (listener->backlog_count >= SOCKET_BACKLOG)
		return NET_SOCK_ERR_WOULDBLOCK;

	socket_t *client = NULL;
	int r = net_socket(&client, AF_INET, SOCK_STREAM, 0);
	if (r != NET_SOCK_OK)
		return r;

	client->inet_data->tcp_conn = conn;
	client->inet_data->local_ip = listener->inet_data->local_ip ?
		listener->inet_data->local_ip : (dev ? dev->ipv4_addr : 0);
	client->inet_data->local_port = listener->inet_data->local_port;
	client->inet_data->remote_ip = remote_ip;
	client->inet_data->remote_port = remote_port;
	client->flags |= NET_SOCK_BINDED | NET_SOCK_CONNECTED;

	listener->backlog[listener->backlog_count++] = client;

	log_debug("socket", "queued TCP accept on port %u from %u.%u.%u.%u:%u",
			  listener->inet_data->local_port, (remote_ip >> 24) & 0xff,
			  (remote_ip >> 16) & 0xff, (remote_ip >> 8) & 0xff,
			  remote_ip & 0xff, remote_port);

	return NET_SOCK_OK;
}

static int udp_port_in_use(uint16_t port)
{
	for (socket_entry_t *entry = socket_list; entry; entry = entry->next) {
		socket_t *s = entry->socket;
		if (!s || s->domain != AF_INET || s->type != SOCK_DGRAM || !s->inet_data)
			continue;
		if (s->inet_data->local_port == port)
			return 1;
	}
	return 0;
}

static int udp_auto_bind(socket_t *sock)
{
	if (!sock || sock->domain != AF_INET || sock->type != SOCK_DGRAM ||
		!sock->inet_data)
		return NET_SOCK_ERR_INVAL;

	if (sock->inet_data->local_port)
		return NET_SOCK_OK;

	for (uint32_t tries = UDP_EPHEMERAL_MIN;
		 tries <= UDP_EPHEMERAL_MAX; tries++) {
		uint16_t port = udp_next_ephemeral++;
		if (udp_next_ephemeral < UDP_EPHEMERAL_MIN)
			udp_next_ephemeral = UDP_EPHEMERAL_MIN;

		if (!udp_port_in_use(port)) {
			sock->inet_data->local_port = port;
			sock->inet_data->local_ip = 0;
			sock->flags |= NET_SOCK_BINDED;
			return NET_SOCK_OK;
		}
	}

	return NET_SOCK_ERR_ADDRINUSE;
}

static int udp_send_packet(socket_t *sock, uint32_t dst_ip, uint16_t dst_port,
						   const void *buf, size_t len)
{
	if (!sock || !sock->inet_data || !buf)
		return NET_SOCK_ERR_INVAL;
	if (len > 1400)
		return NET_SOCK_ERR_MSGSIZE;

	int r = udp_auto_bind(sock);
	if (r != NET_SOCK_OK)
		return r;

	uint32_t tx_dst_ip = dst_ip;
	uint32_t next_hop = 0;
	netdev_t *dev = net_route(tx_dst_ip, &next_hop);
	if (!dev || !dev->ipv4_addr)
		return NET_SOCK_ERR_NETUNREACH;

	tx_dst_ip = net_udp_resolve_remote_ip(dev, dst_ip, dst_port);
	if (tx_dst_ip != dst_ip) {
		log_debug("socket",
				  "UDP DNS using netdev resolver %u.%u.%u.%u instead of %u.%u.%u.%u",
				  (tx_dst_ip >> 24) & 0xff, (tx_dst_ip >> 16) & 0xff,
				  (tx_dst_ip >> 8) & 0xff, tx_dst_ip & 0xff,
				  (dst_ip >> 24) & 0xff, (dst_ip >> 16) & 0xff,
				  (dst_ip >> 8) & 0xff, dst_ip & 0xff);
	}

	dev = net_route(tx_dst_ip, &next_hop);
	if (!dev || !dev->ipv4_addr)
		return NET_SOCK_ERR_NETUNREACH;

	uint8_t mac[NET_ETH_ALEN];
	memset(mac, 0, sizeof(mac));
	if (!dev->loopback && tx_dst_ip != dev->ipv4_addr) {
		r = net_arp_resolve(dev, next_hop ? next_hop : tx_dst_ip, 1000, mac);
		if (r != 0) {
			log_debug("socket", "UDP arp failed dst=%u.%u.%u.%u port=%u",
					  (tx_dst_ip >> 24) & 0xff, (tx_dst_ip >> 16) & 0xff,
					  (tx_dst_ip >> 8) & 0xff, tx_dst_ip & 0xff, dst_port);
			return NET_SOCK_ERR_NETUNREACH;
		}
	}

	uint32_t src_ip = sock->inet_data->local_ip ? sock->inet_data->local_ip :
		dev->ipv4_addr;
	r = net_send_ipv4_udp(dev, mac, src_ip, tx_dst_ip,
						 sock->inet_data->local_port, dst_port, buf, len);
	if (r != 0)
		return r;

	sock->inet_data->local_ip = src_ip;
	sock->flags |= NET_SOCK_BINDED;
	log_debug("socket", "UDP tx %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u len=%zu",
			  (src_ip >> 24) & 0xff, (src_ip >> 16) & 0xff,
			  (src_ip >> 8) & 0xff, src_ip & 0xff,
			  sock->inet_data->local_port,
			  (tx_dst_ip >> 24) & 0xff, (tx_dst_ip >> 16) & 0xff,
			  (tx_dst_ip >> 8) & 0xff, tx_dst_ip & 0xff, dst_port, len);
	return (int)len;
}

static int raw_icmp_send_packet(socket_t *sock, uint32_t dst_ip,
								const void *buf, size_t len)
{
	if (!sock || !sock->inet_data || !buf)
		return NET_SOCK_ERR_INVAL;
	if (len == 0 || len > 1400)
		return NET_SOCK_ERR_MSGSIZE;

	uint32_t next_hop = 0;
	netdev_t *dev = net_route(dst_ip, &next_hop);
	if (!dev || !dev->ipv4_addr)
		return NET_SOCK_ERR_NETUNREACH;

	uint8_t mac[NET_ETH_ALEN];
	memset(mac, 0, sizeof(mac));
	if (!dev->loopback && dst_ip != dev->ipv4_addr) {
		int r = net_arp_resolve(dev, next_hop ? next_hop : dst_ip, 1000, mac);
		if (r != 0)
			return NET_SOCK_ERR_NETUNREACH;
	}

	uint32_t src_ip = sock->inet_data->local_ip ? sock->inet_data->local_ip :
		dev->ipv4_addr;
	int r = net_send_ipv4_icmp(dev, mac, src_ip, dst_ip, buf, len);
	if (r != 0)
		return r;

	sock->inet_data->local_ip = src_ip;
	sock->flags |= NET_SOCK_BINDED;
	return (int)len;
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

		uint32_t local_ip = ntohl(addr->sin.sin_addr);
		uint16_t port = ntohs(addr->sin.sin_port);

		if (!ipv4_addr_is_local(local_ip))
			return NET_SOCK_ERR_ADDRNOTAVAIL;

		if (inet_bind_conflicts(sock, local_ip, port))
			return NET_SOCK_ERR_ADDRINUSE;

		sock->inet_data->local_ip = local_ip;
		sock->inet_data->local_port = port;
		sock->flags |= NET_SOCK_BINDED;
		log_debug("socket", "AF_INET bound to %u.%u.%u.%u:%d",
				  (local_ip >> 24) & 0xff, (local_ip >> 16) & 0xff,
				  (local_ip >> 8) & 0xff, local_ip & 0xff,
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
		if (sock->type != SOCK_STREAM)
			return NET_SOCK_ERR_INVAL;

		if (!sock->inet_data || !sock->inet_data->local_port)
			return NET_SOCK_ERR_INVAL;

		int r = net_tcp_listen_accept_addr(sock->inet_data->local_ip,
											  sock->inet_data->local_port,
											  tcp_socket_accept, sock);
		if (r != 0)
			return r;

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
		if (!(sock->flags & NET_SOCK_LISTENING))
			return NET_SOCK_ERR_INVAL;

		while (sock->backlog_count == 0) {
			if (sock->flags & NET_SOCK_NONBLOCK)
				return NET_SOCK_ERR_WOULDBLOCK;
			if (socket_interrupted() != 0)
				return -EINTR;

			net_poll_all();
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

		if (sock->type == SOCK_DGRAM) {
			int r = udp_auto_bind(sock);
			if (r != NET_SOCK_OK)
				return r;
			sock->flags |= NET_SOCK_CONNECTED;
			log_debug("socket", "AF_INET UDP connected to %u.%u.%u.%u:%d",
					  (sock->inet_data->remote_ip >> 24) & 0xff,
					  (sock->inet_data->remote_ip >> 16) & 0xff,
					  (sock->inet_data->remote_ip >> 8) & 0xff,
					  sock->inet_data->remote_ip & 0xff,
					  sock->inet_data->remote_port);
			return NET_SOCK_OK;
		}

		if (sock->type == SOCK_RAW) {
			sock->flags |= NET_SOCK_CONNECTED;
			return NET_SOCK_OK;
		}

		/* Use net_route so loopback (127.x) goes via lo, not eth0. */
		uint32_t next_hop = 0;
		netdev_t *dev = net_route(sock->inet_data->remote_ip, &next_hop);
		if (!dev)
			return NET_SOCK_ERR_NOENT;

		if (!sock->inet_data->local_ip)
			sock->inet_data->local_ip = dev->ipv4_addr;

		net_tcp_conn_t *conn = NULL;
		int r = net_tcp_connect_ip(dev, sock->inet_data->remote_ip,
								   sock->inet_data->remote_port, &conn, 5000);

		if (r != 0) {
			log_debug("socket", "AF_INET connect failed r=%d", r);
			if (r == -EINTR)
				return r;
			if (r == -ETIMEDOUT)
				return NET_SOCK_ERR_TIMEDOUT;
			return NET_SOCK_ERR_CONNREFUSED;
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
		sched_io_wake_all();

		return (int)to_copy;
	} else if (sock->domain == AF_INET) {
		inet_sock_t *is = sock->inet_data;

		if (sock->type == SOCK_DGRAM) {
			uint32_t dst_ip = 0;
			uint16_t dst_port = 0;

			if (dest && addrlen >= sizeof(sockaddr_in_t)) {
				if (dest->sin.sin_family != AF_INET)
					return NET_SOCK_ERR_INVAL;
				dst_ip = ntohl(dest->sin.sin_addr);
				dst_port = ntohs(dest->sin.sin_port);
			} else if (sock->flags & NET_SOCK_CONNECTED) {
				dst_ip = is->remote_ip;
				dst_port = is->remote_port;
			} else {
				return NET_SOCK_ERR_NOTCONN;
			}

			return udp_send_packet(sock, dst_ip, dst_port, buf, len);
		}

		if (sock->type == SOCK_RAW && sock->protocol == IPPROTO_ICMP) {
			uint32_t dst_ip = 0;

			if (dest && addrlen >= sizeof(sockaddr_in_t)) {
				if (dest->sin.sin_family != AF_INET)
					return NET_SOCK_ERR_INVAL;
				dst_ip = ntohl(dest->sin.sin_addr);
			} else if (sock->flags & NET_SOCK_CONNECTED) {
				dst_ip = is->remote_ip;
			} else {
				return NET_SOCK_ERR_NOTCONN;
			}

			return raw_icmp_send_packet(sock, dst_ip, buf, len);
		}

		if (!is->tcp_conn)
			return NET_SOCK_ERR_NOTCONN;

		if (!(sock->flags & NET_SOCK_CONNECTED))
			return NET_SOCK_ERR_WOULDBLOCK;

		size_t done = 0;
		int r = net_tcp_send(is->tcp_conn, buf, len, &done, 5000);
		if (r != 0)
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
		if (sock->type == SOCK_DGRAM || sock->type == SOCK_RAW) {
			inet_sock_t *is = sock->inet_data;
			if (is->rx_len == 0) {
				time_timeout_t timeout;
				time_timeout_after_ms((int)is->recv_timeout_ms, &timeout);
				while (!is->readable && !time_timeout_expired(&timeout)) {
					if (socket_interrupted() != 0)
						return -EINTR;
					unsigned io_seq = sched_io_wait_prepare();
					net_poll_all();
					if (is->readable)
						break;
					int wr = sched_io_wait(io_seq, &timeout);
					if (wr == -EINTR)
						return -EINTR;
					if (wr == -ETIMEDOUT)
						break;
				}
			}
			if (is->rx_len == 0)
				return NET_SOCK_ERR_WOULDBLOCK;

			size_t to_read = is->rx_len;
			if (to_read > len)
				to_read = len;
			memcpy(buf, is->rx_buf + is->rx_head, to_read);

			if (addr && addrlen && *addrlen >= sizeof(sockaddr_in_t)) {
				memset(addr, 0, sizeof(*addr));
				addr->sin.sin_family = AF_INET;
				addr->sin.sin_addr = htonl(is->packet_src_ip);
				addr->sin.sin_port = htons(is->packet_src_port);
				*addrlen = sizeof(sockaddr_in_t);
			}

			is->rx_len = 0;
			is->rx_head = 0;
			is->readable = 0;
			return (int)to_read;
		}

		if (!sock->inet_data->tcp_conn)
			return NET_SOCK_ERR_NOTCONN;

		if (!(sock->flags & NET_SOCK_CONNECTED))
			return NET_SOCK_ERR_WOULDBLOCK;

		if (sock->inet_data->rx_len == 0) {
			if (sock->flags & NET_SOCK_NONBLOCK)
				return NET_SOCK_ERR_WOULDBLOCK;

			size_t done = 0;
			int r =
				net_tcp_recv(sock->inet_data->tcp_conn, sock->inet_data->rx_buf,
							 SOCKET_BUF_SIZE, &done, 10000);

			if (r == -EINTR)
				return r;
			if (r == -ETIMEDOUT)
				return NET_SOCK_ERR_WOULDBLOCK;

			if (r != 0)
				return NET_SOCK_ERR_NOTCONN;

			/*
			 * done == 0 with 0 is TCP EOF. Return 0 to userspace
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

int net_socket_ref(socket_t *sock)
{
	if (!sock)
		return NET_SOCK_ERR_INVAL;
	sock->refcount++;
	return NET_SOCK_OK;
}

int net_socket_get_status_flags(socket_t *sock, uint32_t *flags)
{
	if (!sock || !flags)
		return NET_SOCK_ERR_INVAL;

	*flags = 0;
	if (sock->flags & NET_SOCK_NONBLOCK)
		*flags |= SOCK_NONBLOCK;

	return NET_SOCK_OK;
}

int net_socket_set_status_flags(socket_t *sock, uint32_t flags)
{
	if (!sock)
		return NET_SOCK_ERR_INVAL;

	if (flags & SOCK_NONBLOCK)
		sock->flags |= NET_SOCK_NONBLOCK;
	else
		sock->flags &= ~NET_SOCK_NONBLOCK;

	return NET_SOCK_OK;
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

	if (sock->domain_ops && sock->domain_ops->destroy)
		sock->domain_ops->destroy(sock);
	else {
		unix_socket_destroy(sock);
		inet_socket_destroy(sock);
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
			sock->inet_data ? htonl(sock->inet_data->local_ip) : 0;
		addr->sin.sin_port =
			sock->inet_data ? htons(sock->inet_data->local_port) : 0;
		*addrlen = sizeof(sockaddr_in_t);
	}

	return NET_SOCK_OK;
}

int net_getpeername(socket_t *sock, sockaddr_t *addr, socklen_t *addrlen)
{
	if (!sock || !addr || !addrlen)
		return NET_SOCK_ERR_INVAL;

	if (sock->domain == AF_UNIX && sock->unix_data) {
		if (!(sock->flags & NET_SOCK_CONNECTED))
			return NET_SOCK_ERR_NOTCONN;
		memset(addr, 0, sizeof(*addr));
		addr->sun.sun_family = AF_UNIX;
		*addrlen = sizeof(sa_family_t_16);
		return NET_SOCK_OK;
	}

	if (sock->domain == AF_INET && sock->inet_data) {
		if (!(sock->flags & NET_SOCK_CONNECTED))
			return NET_SOCK_ERR_NOTCONN;
		memset(addr, 0, sizeof(*addr));
		addr->sin.sin_family = AF_INET;
		addr->sin.sin_addr = htonl(sock->inet_data->remote_ip);
		addr->sin.sin_port = htons(sock->inet_data->remote_port);
		*addrlen = sizeof(sockaddr_in_t);
		return NET_SOCK_OK;
	}

	return NET_SOCK_ERR_INVAL;
}

int net_setsockopt(socket_t *sock, int level, int optname, const void *optval,
				   socklen_t optlen)
{
	if (!sock || !optval)
		return NET_SOCK_ERR_INVAL;

	if (sock->domain == AF_INET && sock->inet_data && level == SOL_SOCKET &&
		optname == SO_RCVTIMEO) {
		if (optlen < sizeof(socket_timeval_t))
			return NET_SOCK_ERR_INVAL;

		const socket_timeval_t *tv = optval;
		if (tv->tv_sec < 0 || tv->tv_usec < 0)
			return NET_SOCK_ERR_INVAL;

		sock->inet_data->recv_timeout_ms =
			(uint64_t)tv->tv_sec * 1000 + (uint64_t)tv->tv_usec / 1000;
		return NET_SOCK_OK;
	}

	if (sock->domain == AF_INET && sock->inet_data && level == IPPROTO_IP &&
		optname == IP_TTL) {
		if (optlen < sizeof(int))
			return NET_SOCK_ERR_INVAL;
		int ttl = *(const int *)optval;
		if (ttl <= 0 || ttl > 255)
			return NET_SOCK_ERR_INVAL;
		sock->inet_data->ttl = ttl;
		return NET_SOCK_OK;
	}

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

int net_poll_socket(socket_t *sock, int events)
{
	if (!sock)
		return LYR_POLLNVAL;

	int revents = 0;

	if (sock->domain == AF_UNIX && sock->unix_data) {
		unix_sock_t *us = sock->unix_data;

		if ((events & LYR_POLL_READ_MASK) && us->rx_len > 0)
			revents |= LYR_POLLIN | LYR_POLLRDNORM;

		if ((events & LYR_POLL_WRITE_MASK) &&
			(us->rx_cap == 0 || us->rx_len < us->rx_cap))
			revents |= LYR_POLLOUT | LYR_POLLWRNORM;

		return revents;
	}

	if (sock->domain == AF_INET && sock->inet_data) {
		inet_sock_t *is = sock->inet_data;

		if ((events & LYR_POLL_READ_MASK) && is->rx_len > 0)
			revents |= LYR_POLLIN | LYR_POLLRDNORM;

		if (sock->type == SOCK_STREAM && is->tcp_conn) {
			/*
			 * Socket-layer TCP buffering is filled by recv().  Until the TCP core
			 * exposes a non-consuming readiness primitive, report established
			 * TCP sockets as writable and rely on recv() for blocking reads.
			 */
			if (!(sock->flags & NET_SOCK_CONNECTED))
				revents |= LYR_POLLHUP;
		}

		if ((events & LYR_POLL_WRITE_MASK) &&
			(sock->type == SOCK_DGRAM || sock->type == SOCK_RAW ||
			 (sock->flags & NET_SOCK_CONNECTED)))
			revents |= LYR_POLLOUT | LYR_POLLWRNORM;

		return revents;
	}

	return LYR_POLLERR;
}

void net_socket_raw_icmp_receive(netdev_t *dev, const ipv4_hdr_t *ip,
								 size_t ihl, size_t ip_len)
{
	(void)dev;
	if (!ip || ip_len < ihl)
		return;

	uint32_t src_ip = ntohl(ip->src);
	uint32_t dst_ip = ntohl(ip->dst);

	if (ip_len >= ihl + sizeof(icmp_echo_t)) {
		const icmp_echo_t *icmp =
			(const icmp_echo_t *)((const uint8_t *)ip + ihl);

		/*
		 * Loopback sends our own echo request back through RX before the echo
		 * reply is generated.  Do not let that self-originated request occupy
		 * the one-packet raw receive slot; otherwise userspace ping can read the
		 * request, discard it, and miss the reply that was dropped while the slot
		 * was full.
		 */
		if (dev && dev->loopback && src_ip == dst_ip && icmp->type == 8)
			return;
	}

	for (socket_entry_t *entry = socket_list; entry; entry = entry->next) {
		socket_t *sock = entry->socket;
		if (!sock || sock->domain != AF_INET || sock->type != SOCK_RAW ||
			sock->protocol != IPPROTO_ICMP || !sock->inet_data)
			continue;

		inet_sock_t *is = sock->inet_data;
		if (is->local_ip != 0 && is->local_ip != dst_ip)
			continue;

		if ((sock->flags & NET_SOCK_CONNECTED) && is->remote_ip != src_ip)
			continue;

		if (ip_len > sizeof(is->rx_buf))
			ip_len = sizeof(is->rx_buf);

		/* Single-packet receive queue for now. Drop if userspace is behind. */
		if (is->rx_len != 0)
			continue;

		memcpy(is->rx_buf, ip, ip_len);
		is->rx_len = ip_len;
		is->rx_head = 0;
		is->packet_src_ip = src_ip;
		is->packet_src_port = 0;
		is->readable = 1;
		sched_io_wake_all();
	}
}

int socket_init(void)
{
	socket_register_builtin_domains();
	(void)net_udp_register_handler(&socket_udp_handler);
	log_info("socket", "initializing socket layer");
	return NET_SOCK_OK;
}

static int socket_udp_receive(const net_udp_dgram_t *dgram, void *ctx)
{
	(void)ctx;

	if (!dgram)
		return NET_SOCK_ERR_INVAL;

	for (socket_entry_t *entry = socket_list; entry; entry = entry->next) {
		socket_t *sock = entry->socket;
		if (!sock || sock->domain != AF_INET || sock->type != SOCK_DGRAM ||
			!sock->inet_data)
			continue;

		inet_sock_t *is = sock->inet_data;
		if (is->local_port != dgram->dst_port)
			continue;

		if (is->local_ip != 0 && is->local_ip != dgram->dst_ip)
			continue;

		if (sock->flags & NET_SOCK_CONNECTED) {
			if (!net_udp_peer_matches(dgram->ipv4 ? dgram->ipv4->dev : NULL,
									  is->remote_ip, is->remote_port,
									  dgram->src_ip, dgram->src_port))
				continue;
		}

		size_t payload_len = dgram->payload_len;
		if (payload_len > sizeof(is->rx_buf))
			payload_len = sizeof(is->rx_buf);

		/* Single-packet receive queue for now. Drop if userspace is behind. */
		if (is->rx_len != 0)
			return 0;

		memcpy(is->rx_buf, dgram->payload, payload_len);
		is->rx_len = payload_len;
		is->rx_head = 0;
		is->packet_src_ip = dgram->src_ip;
		is->packet_src_port = dgram->src_port;
		is->readable = 1;
		sched_io_wake_all();

		log_debug("socket", "UDP rx %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u len=%zu",
				  (dgram->src_ip >> 24) & 0xff, (dgram->src_ip >> 16) & 0xff,
				  (dgram->src_ip >> 8) & 0xff, dgram->src_ip & 0xff,
				  dgram->src_port, (dgram->dst_ip >> 24) & 0xff,
				  (dgram->dst_ip >> 16) & 0xff, (dgram->dst_ip >> 8) & 0xff,
				  dgram->dst_ip & 0xff, dgram->dst_port,
				  payload_len);
		return 1;
	}

	return 0;
}
