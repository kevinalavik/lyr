#include "internal.h"
#include <debug/log.h>
#include <dev/pit.h>
#include <fs/vfs.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <net/net.h>

#define TCP_RST 0x04

#define TCP_PASSIVE_RESPONSE_CAP 32768
#define TCP_ACTIVE_RX_CAP 32768

typedef struct tcp_conn {
	netdev_t *dev;
	uint32_t remote_ip;
	uint16_t remote_port;
	uint16_t local_port;
	uint32_t seq;
	uint32_t ack;
	uint8_t peer_mac[NET_ETH_ALEN];
	char *rx_buf;
	size_t rx_cap;
	size_t rx_len;
	net_tcp_listen_handler_t handler;
	net_tcp_accept_handler_t accept_handler;
	void *handler_ctx;
	int passive;
	int connected;
	int closed;
	int error;
	int heap_allocated;
	struct tcp_conn *next;
} tcp_conn_t;

typedef struct tcp_listener {
	uint32_t local_ip;
	uint16_t port;
	net_tcp_listen_handler_t handler;
	net_tcp_accept_handler_t accept_handler;
	void *ctx;
	struct tcp_listener *next;
} tcp_listener_t;

static tcp_conn_t *tcp_conns;
static tcp_listener_t *tcp_listeners;

static void tcp_conn_add(tcp_conn_t *conn)
{
	conn->next = tcp_conns;
	tcp_conns = conn;
}

static void tcp_conn_remove(tcp_conn_t *conn)
{
	tcp_conn_t **cur = &tcp_conns;
	while (*cur) {
		if (*cur == conn) {
			*cur = conn->next;
			conn->next = NULL;
			return;
		}
		cur = &(*cur)->next;
	}
}

static tcp_conn_t *tcp_conn_find(netdev_t *dev, const ipv4_hdr_t *ip,
								 const tcp_hdr_t *tcp)
{
	uint32_t remote_ip = ntohl(ip->src);
	uint16_t remote_port = ntohs(tcp->src_port);
	uint16_t local_port = ntohs(tcp->dst_port);

	for (tcp_conn_t *conn = tcp_conns; conn; conn = conn->next) {
		if (conn->dev == dev && conn->remote_ip == remote_ip &&
			conn->remote_port == remote_port && conn->local_port == local_port)
			return conn;
	}
	return NULL;
}

static tcp_listener_t *tcp_listener_find(uint32_t local_ip, uint16_t port)
{
	tcp_listener_t *wildcard = NULL;

	for (tcp_listener_t *listener = tcp_listeners; listener;
		 listener = listener->next) {
		if (listener->port != port)
			continue;

		/* Linux-style bind semantics: an exact local address wins over
		 * INADDR_ANY. A wildcard listener accepts packets for any local
		 * address only if there is no exact listener for that address. */
		if (listener->local_ip == local_ip)
			return listener;
		if (listener->local_ip == 0)
			wildcard = listener;
	}

	return wildcard;
}

static void tcp_send_reset(netdev_t *dev, const uint8_t src_mac[NET_ETH_ALEN],
						   const ipv4_hdr_t *ip, const tcp_hdr_t *tcp)
{
	uint8_t flags = TCP_RST;
	uint32_t seq = 0;
	uint32_t ack = ntohl(tcp->seq) + 1;
	if (tcp->flags & TCP_ACK) {
		seq = ntohl(tcp->ack);
		ack = 0;
		flags |= 0;
	} else {
		flags |= TCP_ACK;
	}

	net_send_ipv4_tcp(dev, src_mac, ntohl(ip->src), ntohs(tcp->dst_port),
					  ntohs(tcp->src_port), seq, ack, flags, NULL, 0);
}

static void tcp_handle_listen_syn(netdev_t *dev,
								  const uint8_t src_mac[NET_ETH_ALEN],
								  const ipv4_hdr_t *ip, const tcp_hdr_t *tcp,
								  tcp_listener_t *listener)
{
	tcp_conn_t *conn = kzalloc(sizeof(*conn));
	if (!conn) {
		tcp_send_reset(dev, src_mac, ip, tcp);
		return;
	}

	conn->dev = dev;
	conn->remote_ip = ntohl(ip->src);
	conn->remote_port = ntohs(tcp->src_port);
	conn->local_port = ntohs(tcp->dst_port);
	conn->seq = 0x4c595300u + (uint32_t)(pit_get_ticks() & 0xffff);
	conn->ack = ntohl(tcp->seq) + 1;
	memcpy(conn->peer_mac, src_mac, NET_ETH_ALEN);
	conn->handler = listener->handler;
	conn->accept_handler = listener->accept_handler;
	conn->handler_ctx = listener->ctx;
	conn->passive = 1;

	if (conn->accept_handler) {
		conn->rx_cap = TCP_ACTIVE_RX_CAP;
		conn->rx_buf = kzalloc(conn->rx_cap);
		if (!conn->rx_buf) {
			kfree(conn);
			tcp_send_reset(dev, src_mac, ip, tcp);
			return;
		}
	}
	conn->heap_allocated = 1;
	tcp_conn_add(conn);

	net_send_ipv4_tcp(dev, conn->peer_mac, conn->remote_ip, conn->local_port,
					  conn->remote_port, conn->seq, conn->ack,
					  TCP_SYN | TCP_ACK, NULL, 0);
}

static void tcp_close_passive(tcp_conn_t *conn)
{
	tcp_conn_remove(conn);
	if (conn->rx_buf)
		kfree(conn->rx_buf);
	kfree(conn);
}

static int tcp_send_payload(tcp_conn_t *conn, const void *payload, size_t len)
{
	const uint8_t *p = payload;

	while (len) {
		size_t chunk = len > 1400 ? 1400 : len;
		uint32_t seq = conn->seq;

		conn->seq += (uint32_t)chunk;

		log_debug(
			"tcp",
			"send payload: %u.%u.%u.%u:%u -> local:%u seq=%u ack=%u len=%zu",
			(conn->remote_ip >> 24) & 0xff, (conn->remote_ip >> 16) & 0xff,
			(conn->remote_ip >> 8) & 0xff, conn->remote_ip & 0xff,
			conn->remote_port, conn->local_port, seq, conn->ack, chunk);

		int r = net_send_ipv4_tcp(conn->dev, conn->peer_mac, conn->remote_ip,
								  conn->local_port, conn->remote_port, seq,
								  conn->ack, TCP_PSH | TCP_ACK, p, chunk);
		if (r != VFS_OK) {
			conn->seq = seq;
			return r;
		}

		p += chunk;
		len -= chunk;
	}

	return VFS_OK;
}

static void tcp_handle_passive_payload(tcp_conn_t *conn, const uint8_t *payload,
									   size_t payload_len)
{
	if (!conn->handler)
		return;

	char *response = kzalloc(TCP_PASSIVE_RESPONSE_CAP);
	if (!response)
		return;

	size_t response_len = 0;

	log_debug("tcp", "passive payload: local_port=%u remote_port=%u len=%zu",
			  conn->local_port, conn->remote_port, payload_len);

	int r = conn->handler(
		conn->dev, conn->remote_ip, conn->remote_port, payload, payload_len,
		response, TCP_PASSIVE_RESPONSE_CAP, &response_len, conn->handler_ctx);
	log_debug("tcp", "passive handler returned r=%d response_len=%zu", r,
			  response_len);
	if (r != VFS_OK)
		response_len = 0;

	if (response_len > TCP_PASSIVE_RESPONSE_CAP)
		response_len = TCP_PASSIVE_RESPONSE_CAP;

	if (response_len) {
		if (tcp_send_payload(conn, response, response_len) != VFS_OK)
			response_len = 0;
	}

	kfree(response);

	uint32_t fin_seq = conn->seq;
	conn->seq++;

	if (net_send_ipv4_tcp(conn->dev, conn->peer_mac, conn->remote_ip,
						  conn->local_port, conn->remote_port, fin_seq,
						  conn->ack, TCP_FIN | TCP_ACK, NULL, 0) != VFS_OK) {
		conn->seq = fin_seq;
		return;
	}

	conn->closed = 1;
}

void net_tcp_receive(netdev_t *dev, const uint8_t src_mac[NET_ETH_ALEN],
					 const ipv4_hdr_t *ip, size_t ihl, size_t ip_len)
{
	if (ip_len < ihl + sizeof(tcp_hdr_t))
		return;

	const tcp_hdr_t *tcp = (const tcp_hdr_t *)((const uint8_t *)ip + ihl);
	tcp_conn_t *conn = tcp_conn_find(dev, ip, tcp);
	if (!conn) {
		uint16_t local_port = ntohs(tcp->dst_port);
		uint32_t local_ip = ntohl(ip->dst);
		tcp_listener_t *listener = tcp_listener_find(local_ip, local_port);
		log_debug(
			"tcp",
			"rx no conn: src=%u.%u.%u.%u:%u dst_port=%u flags=0x%x listener=%s",
			(ntohl(ip->src) >> 24) & 0xff, (ntohl(ip->src) >> 16) & 0xff,
			(ntohl(ip->src) >> 8) & 0xff, ntohl(ip->src) & 0xff,
			ntohs(tcp->src_port), local_port, tcp->flags,
			listener ? "yes" : "no");
		if (listener && (tcp->flags & TCP_SYN)) {
			tcp_handle_listen_syn(dev, src_mac, ip, tcp, listener);
			return;
		}
		if (!listener)
			tcp_send_reset(dev, src_mac, ip, tcp);
		return;
	}

	size_t tcp_hlen = (tcp->data_off >> 4) * 4;
	if (tcp_hlen < sizeof(*tcp) || ip_len < ihl + tcp_hlen)
		return;

	uint8_t flags = tcp->flags;
	uint32_t seq = ntohl(tcp->seq);
	uint32_t ack = ntohl(tcp->ack);
	size_t payload_len = ip_len - ihl - tcp_hlen;
	const uint8_t *payload = (const uint8_t *)tcp + tcp_hlen;

	log_debug(
		"tcp",
		"rx: %u.%u.%u.%u:%u -> local:%u flags=0x%x seq=%u ack=%u payload=%zu passive=%d connected=%d closed=%d rx_len=%zu rx_cap=%zu",
		(ntohl(ip->src) >> 24) & 0xff, (ntohl(ip->src) >> 16) & 0xff,
		(ntohl(ip->src) >> 8) & 0xff, ntohl(ip->src) & 0xff,
		ntohs(tcp->src_port), ntohs(tcp->dst_port), flags, seq, ack,
		payload_len, conn->passive, conn->connected, conn->closed, conn->rx_len,
		conn->rx_cap);

	if (flags & 0x04) {
		conn->error = 1;
		conn->closed = 1;
		if (conn->passive)
			tcp_close_passive(conn);
		return;
	}

	if (conn->passive) {
		if (!conn->connected && (flags & TCP_ACK) && ack == conn->seq + 1) {
			conn->seq++;
			conn->connected = 1;

			if (conn->accept_handler) {
				int r = conn->accept_handler(conn->dev, conn->remote_ip,
											 conn->remote_port, conn,
											 conn->handler_ctx);
				if (r != VFS_OK) {
					tcp_close_passive(conn);
					return;
				}

				/* Hand accepted sockets to the normal connected TCP path. */
				conn->passive = 0;
			}
		}

		if (conn->passive) {
			if (!conn->connected || seq != conn->ack)
				return;

			if (payload_len) {
				conn->ack += (uint32_t)payload_len;
				tcp_handle_passive_payload(conn, payload, payload_len);
			} else if (flags & TCP_FIN) {
				conn->ack++;
				net_send_ipv4_tcp(dev, conn->peer_mac, conn->remote_ip,
								  conn->local_port, conn->remote_port,
								  conn->seq, conn->ack, TCP_ACK, NULL, 0);
				tcp_close_passive(conn);
			} else if (conn->closed && (flags & TCP_ACK)) {
				tcp_close_passive(conn);
			}
			return;
		}
	}

	if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
		ack == conn->seq + 1) {
		conn->ack = seq + 1;
		conn->seq++;
		conn->connected = 1;
		net_send_ipv4_tcp(dev, conn->peer_mac, conn->remote_ip,
						  conn->local_port, conn->remote_port, conn->seq,
						  conn->ack, TCP_ACK, NULL, 0);
		return;
	}

	if (!conn->connected || seq != conn->ack)
		return;

	if (payload_len) {
		size_t copy = payload_len;
		if (!conn->rx_buf || conn->rx_len >= conn->rx_cap)
			copy = 0;
		else if (copy > conn->rx_cap - conn->rx_len)
			copy = conn->rx_cap - conn->rx_len;
		if (copy) {
			memcpy(conn->rx_buf + conn->rx_len, payload, copy);
			conn->rx_len += copy;
		}
		log_debug(
			"tcp",
			"active queued payload: got=%zu copied=%zu queued=%zu cap=%zu",
			payload_len, copy, conn->rx_len, conn->rx_cap);
		conn->ack += (uint32_t)payload_len;
		net_send_ipv4_tcp(dev, conn->peer_mac, conn->remote_ip,
						  conn->local_port, conn->remote_port, conn->seq,
						  conn->ack, TCP_ACK, NULL, 0);
	}

	if (flags & TCP_FIN) {
		conn->ack++;
		net_send_ipv4_tcp(dev, conn->peer_mac, conn->remote_ip,
						  conn->local_port, conn->remote_port, conn->seq,
						  conn->ack, TCP_ACK, NULL, 0);
		conn->closed = 1;
	}
}

int net_tcp_connect_ip(netdev_t *dev, uint32_t dst_ip, uint16_t port,
					   net_tcp_conn_t **out, uint64_t timeout_ms)
{
	if (!dev || !dst_ip || !port || !out)
		return VFS_ERR_INVAL;

	log_debug("tcp", "connect: dst_ip=%x port=%d timeout=%llu", dst_ip, port,
			  timeout_ms);

	uint32_t next_hop = dst_ip;
	if ((dst_ip & dev->ipv4_netmask) != (dev->ipv4_addr & dev->ipv4_netmask)) {
		if (!dev->ipv4_gateway) {
			log_debug("tcp", "connect: no gateway for off-link destination");
			return VFS_ERR_NOENT;
		}
		next_hop = dev->ipv4_gateway;
	}

	log_debug("tcp", "connect: dev=%s src=%u.%u.%u.%u next_hop=%x", dev->name,
			  (dev->ipv4_addr >> 24) & 0xff, (dev->ipv4_addr >> 16) & 0xff,
			  (dev->ipv4_addr >> 8) & 0xff, dev->ipv4_addr & 0xff, next_hop);

	tcp_conn_t *conn = kzalloc(sizeof(*conn));
	if (!conn)
		return VFS_ERR_NOMEM;

	conn->dev = dev;
	conn->remote_ip = dst_ip;
	conn->remote_port = port;
	conn->local_port = 40000 + (uint16_t)(pit_get_ticks() & 0x3fff);
	conn->seq = 0x4c595200u + (uint32_t)(pit_get_ticks() & 0xffff);
	conn->heap_allocated = 1;
	conn->rx_cap = TCP_ACTIVE_RX_CAP;
	conn->rx_buf = kzalloc(conn->rx_cap);
	if (!conn->rx_buf) {
		kfree(conn);
		return VFS_ERR_NOMEM;
	}

	log_debug("tcp", "connect: calling arp_resolve");
	int r = net_arp_resolve(dev, next_hop, timeout_ms, conn->peer_mac);
	log_debug("tcp", "connect: arp_resolve returned %d", r);
	if (r != VFS_OK) {
		if (conn->rx_buf)
			kfree(conn->rx_buf);
		kfree(conn);
		return r;
	}

	tcp_conn_add(conn);

	log_debug("tcp", "connect: sending SYN");
	r = net_send_ipv4_tcp(dev, conn->peer_mac, dst_ip, conn->local_port, port,
						  conn->seq, 0, TCP_SYN, NULL, 0);
	if (r != VFS_OK) {
		log_debug("tcp", "connect: send SYN failed r=%d", r);
		tcp_conn_remove(conn);
		if (conn->rx_buf)
			kfree(conn->rx_buf);
		kfree(conn);
		return r;
	}

	/* Spin-poll until the SYN-ACK arrives and the 3-way handshake
	 * completes, or we time out. net_poll_until drives the NIC rx path. */
	uint64_t until = pit_get_ticks() + net_timeout_ticks(timeout_ms);
	net_poll_until(dev, until, &conn->connected);

	if (conn->error) {
		log_debug("tcp", "connect: reset by peer");
		tcp_conn_remove(conn);
		if (conn->rx_buf)
			kfree(conn->rx_buf);
		kfree(conn);
		return VFS_ERR_NOENT;
	}

	if (!conn->connected) {
		log_debug("tcp", "connect: timed out waiting for SYN-ACK");
		tcp_conn_remove(conn);
		if (conn->rx_buf)
			kfree(conn->rx_buf);
		kfree(conn);
		return VFS_ERR_TIMEOUT;
	}

	log_debug("tcp", "connect: 3-way handshake complete");
	*out = conn;
	return VFS_OK;
}

int net_tcp_connect(const char *host, uint16_t port, net_tcp_conn_t **out,
					uint64_t timeout_ms)
{
	if (!host || !port || !out)
		return VFS_ERR_INVAL;

	netdev_t *dev = net_default_dev();
	if (!dev)
		return VFS_ERR_NOENT;

	uint32_t ip = 0;
	int r = net_dns_resolve_dev(dev, host, timeout_ms, &ip);
	if (r != VFS_OK)
		return r;

	return net_tcp_connect_ip(dev, ip, port, out, timeout_ms);
}

int net_tcp_send(net_tcp_conn_t *conn_, const void *buf, size_t len,
				 size_t *done, uint64_t timeout_ms)
{
	(void)timeout_ms;

	tcp_conn_t *conn = conn_;

	if (!conn || !buf)
		return VFS_ERR_INVAL;

	if (!conn->connected || conn->closed || conn->error)
		return VFS_ERR_NOENT;

	int r = tcp_send_payload(conn, buf, len);
	if (r != VFS_OK)
		return r;

	if (done)
		*done = len;

	return VFS_OK;
}

int net_tcp_recv(net_tcp_conn_t *conn_, void *buf, size_t cap, size_t *done,
				 uint64_t timeout_ms)
{
	tcp_conn_t *conn = conn_;

	if (!conn || !buf || !cap)
		return VFS_ERR_INVAL;

	if (done)
		*done = 0;

	if (!conn->connected)
		return VFS_ERR_NOENT;

	uint64_t until = pit_get_ticks() + net_timeout_ticks(timeout_ms);

	log_debug("tcp",
			  "recv wait: cap=%zu queued=%zu closed=%d error=%d timeout=%llu",
			  cap, conn->rx_len, conn->closed, conn->error, timeout_ms);

	while (!conn->rx_len && !conn->closed && !conn->error &&
		   pit_get_ticks() < until) {
		net_poll_all();
	}

	/*
	 * Data wins over connection errors.
	 *
	 * A peer can legally send payload and then reset the connection. If the RST
	 * is processed before userspace calls read(), conn->error may be set while
	 * conn->rx_len still contains valid data. Return the queued bytes first;
	 * report the reset only after the receive queue has been drained.
	 */
	if (conn->rx_len) {
		size_t copy = conn->rx_len;

		if (copy > cap)
			copy = cap;

		memcpy(buf, conn->rx_buf, copy);

		if (copy < conn->rx_len)
			memmove(conn->rx_buf, conn->rx_buf + copy, conn->rx_len - copy);

		conn->rx_len -= copy;

		if (done)
			*done = copy;

		log_debug("tcp", "recv: copied=%zu remaining=%zu closed=%d error=%d",
				  copy, conn->rx_len, conn->closed, conn->error);

		return VFS_OK;
	}

	if (conn->error) {
		log_debug("tcp", "recv: connection error with empty rx queue");
		return VFS_ERR_NOENT;
	}

	if (conn->closed) {
		log_debug("tcp", "recv: clean EOF");
		return VFS_OK;
	}

	log_debug("tcp", "recv: timeout/no data closed=%d error=%d", conn->closed,
			  conn->error);
	return VFS_ERR_TIMEOUT;
}

void net_tcp_close(net_tcp_conn_t *conn_)
{
	tcp_conn_t *conn = conn_;

	if (!conn)
		return;

	if (conn->connected && !conn->closed && !conn->error) {
		uint32_t fin_seq = conn->seq;
		conn->seq++;

		net_send_ipv4_tcp(conn->dev, conn->peer_mac, conn->remote_ip,
						  conn->local_port, conn->remote_port, fin_seq,
						  conn->ack, TCP_FIN | TCP_ACK, NULL, 0);
	}

	tcp_conn_remove(conn);

	if (conn->rx_buf)
		kfree(conn->rx_buf);

	if (conn->heap_allocated)
		kfree(conn);
}

int net_tcp_http_request(netdev_t *dev, uint32_t dst_ip, const char *host,
						 const char *path, char *buf, size_t len, size_t *done,
						 uint64_t timeout_ms)
{
	if (!dev || !host || !path || !buf || len == 0)
		return VFS_ERR_INVAL;

	uint32_t next_hop = dst_ip;
	if ((dst_ip & dev->ipv4_netmask) != (dev->ipv4_addr & dev->ipv4_netmask)) {
		if (!dev->ipv4_gateway)
			return VFS_ERR_NOENT;
		next_hop = dev->ipv4_gateway;
	}

	tcp_conn_t conn;
	memset(&conn, 0, sizeof(conn));
	conn.dev = dev;
	conn.remote_ip = dst_ip;
	conn.remote_port = 80;
	conn.local_port = 40000 + (uint16_t)(pit_get_ticks() & 0x3fff);
	conn.seq = 0x4c595200u + (uint32_t)(pit_get_ticks() & 0xffff);
	conn.rx_buf = buf;
	conn.rx_cap = len;

	int r = net_arp_resolve(dev, next_hop, timeout_ms, conn.peer_mac);
	if (r != VFS_OK)
		return r;

	tcp_conn_add(&conn);

	r = net_send_ipv4_tcp(dev, conn.peer_mac, dst_ip, conn.local_port, 80,
						  conn.seq, 0, TCP_SYN, NULL, 0);
	if (r != VFS_OK)
		goto out;
	uint64_t until = pit_get_ticks() + net_timeout_ticks(timeout_ms);
	net_poll_until(dev, until, &conn.connected);
	if (!conn.connected || conn.error) {
		r = VFS_ERR_NOENT;
		goto out;
	}

	char req[512];
	int n = npf_snprintf(
		req, sizeof(req),
		"GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\nUser-Agent: lyr/0\r\n\r\n",
		path, host);
	if (n < 0 || (size_t)n >= sizeof(req)) {
		r = VFS_ERR_INVAL;
		goto out;
	}

	r = net_send_ipv4_tcp(dev, conn.peer_mac, dst_ip, conn.local_port, 80,
						  conn.seq, conn.ack, TCP_PSH | TCP_ACK, req,
						  (size_t)n);
	if (r != VFS_OK)
		goto out;
	conn.seq += (uint32_t)n;

	until = pit_get_ticks() + net_timeout_ticks(timeout_ms);
	net_poll_until(dev, until, &conn.closed);
	if (conn.error) {
		r = VFS_ERR_NOENT;
		goto out;
	}
	if (done)
		*done = conn.rx_len;
	r = conn.rx_len ? VFS_OK : VFS_ERR_NOENT;

out:
	tcp_conn_remove(&conn);
	return r;
}

static int tcp_listener_conflicts(uint32_t local_ip, uint16_t port)
{
	for (tcp_listener_t *listener = tcp_listeners; listener;
		 listener = listener->next) {
		if (listener->port != port)
			continue;

		/* INADDR_ANY conflicts with every listener on that port. Exact binds
		 * conflict only with the same exact address or with a wildcard bind. */
		if (listener->local_ip == 0 || local_ip == 0 ||
			listener->local_ip == local_ip)
			return 1;
	}

	return 0;
}

static int tcp_listener_add(uint32_t local_ip, uint16_t port,
							net_tcp_listen_handler_t handler,
							net_tcp_accept_handler_t accept_handler, void *ctx)
{
	if (!port || (!handler && !accept_handler))
		return VFS_ERR_INVAL;

	if (tcp_listener_conflicts(local_ip, port))
		return VFS_ERR_EXIST;

	tcp_listener_t *listener = kzalloc(sizeof(*listener));
	if (!listener)
		return VFS_ERR_NOMEM;
	listener->local_ip = local_ip;
	listener->port = port;
	listener->handler = handler;
	listener->accept_handler = accept_handler;
	listener->ctx = ctx;
	listener->next = tcp_listeners;
	tcp_listeners = listener;
	log_debug("tcp", "listening on %u.%u.%u.%u:%u", (local_ip >> 24) & 0xff,
			  (local_ip >> 16) & 0xff, (local_ip >> 8) & 0xff, local_ip & 0xff,
			  port);
	return VFS_OK;
}

int net_tcp_listen(uint16_t port, net_tcp_listen_handler_t handler, void *ctx)
{
	return tcp_listener_add(0, port, handler, NULL, ctx);
}

int net_tcp_listen_addr(uint32_t local_ip, uint16_t port,
						net_tcp_listen_handler_t handler, void *ctx)
{
	return tcp_listener_add(local_ip, port, handler, NULL, ctx);
}

int net_tcp_listen_accept(uint16_t port, net_tcp_accept_handler_t handler,
						  void *ctx)
{
	return tcp_listener_add(0, port, NULL, handler, ctx);
}

int net_tcp_listen_accept_addr(uint32_t local_ip, uint16_t port,
							   net_tcp_accept_handler_t handler, void *ctx)
{
	return tcp_listener_add(local_ip, port, NULL, handler, ctx);
}
