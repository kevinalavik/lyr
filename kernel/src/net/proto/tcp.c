#include "../stack.h"
#include <debug/log.h>
#include <dev/pit.h>
#include <dev/time.h>
#include <errno.h>
#include <fs/vfs.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <net/net.h>
#include <sched/sched.h>

#define TCP_RST 0x04

#define TCP_PASSIVE_RESPONSE_CAP 32768
#define TCP_ACTIVE_RX_CAP (4 * 1024 * 1024)
#define TCP_RCVBUF_MIN (64 * 1024)
#define TCP_RCVBUF_MAX (4 * 1024 * 1024)
#define TCP_RCVBUF_GROW_THRESHOLD 3
#define TCP_RCV_WSCALE 6
#define TCP_DELAYED_ACK_SEGMENTS 2
#define TCP_SEGMENT_DATA_MAX 1460

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
	size_t rx_head;
	uint8_t rcv_wscale;
	uint8_t snd_wscale;
	int peer_wscale_seen;
	int sack_permitted;
	unsigned rx_since_ack;
	unsigned rx_full_events;
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

static int tcp_ipv4_receive(const net_ipv4_rx_info_t *rx, void *ctx);

static const net_ipv4_protocol_ops_t tcp_ipv4_protocol = {
	.protocol = IP_PROTO_TCP,
	.name = "tcp",
	.receive = tcp_ipv4_receive,
	.ctx = NULL,
};

static int tcp_interrupted(void)
{
	tcb_t *thread = sched_current();
	return (thread && sched_signal_is_pending(thread)) ? -EINTR : 0;
}

static size_t tcp_recv_window_full(const tcp_conn_t *conn)
{
	if (!conn || !conn->rx_buf || conn->rx_len >= conn->rx_cap)
		return 0;

	return conn->rx_cap - conn->rx_len;
}

static uint16_t tcp_recv_window(const tcp_conn_t *conn)
{
	size_t free = tcp_recv_window_full(conn);

	if (conn && conn->peer_wscale_seen && conn->rcv_wscale)
		free >>= conn->rcv_wscale;

	if (free > 65535)
		free = 65535;
	return (uint16_t)free;
}

static size_t tcp_rx_tail(const tcp_conn_t *conn)
{
	return (conn->rx_head + conn->rx_len) % conn->rx_cap;
}

static void tcp_rx_push(tcp_conn_t *conn, const uint8_t *payload,
					size_t payload_len)
{
	size_t tail = tcp_rx_tail(conn);
	size_t first = conn->rx_cap - tail;

	if (first > payload_len)
		first = payload_len;

	memcpy(conn->rx_buf + tail, payload, first);
	if (payload_len > first)
		memcpy(conn->rx_buf, payload + first, payload_len - first);

	conn->rx_len += payload_len;
}

static void tcp_rx_pop(tcp_conn_t *conn, void *buf, size_t len)
{
	size_t first = conn->rx_cap - conn->rx_head;

	if (first > len)
		first = len;

	memcpy(buf, conn->rx_buf + conn->rx_head, first);
	if (len > first)
		memcpy((uint8_t *)buf + first, conn->rx_buf, len - first);

	conn->rx_head = (conn->rx_head + len) % conn->rx_cap;
	conn->rx_len -= len;
}

static uint8_t tcp_wscale_for_cap(size_t cap)
{
	uint8_t shift = 0;

	while (shift < 14 && (cap >> shift) > 65535)
		shift++;

	return shift;
}

static void tcp_parse_syn_options(tcp_conn_t *conn, const tcp_hdr_t *tcp,
						size_t tcp_hlen)
{
	if (!conn || tcp_hlen <= sizeof(*tcp))
		return;

	const uint8_t *opt = (const uint8_t *)tcp + sizeof(*tcp);
	size_t left = tcp_hlen - sizeof(*tcp);

	while (left) {
		uint8_t kind = opt[0];

		if (kind == 0)
			break;
		if (kind == 1) {
			opt++;
			left--;
			continue;
		}
		if (left < 2 || opt[1] < 2 || opt[1] > left)
			break;

		if (kind == 3 && opt[1] == 3) {
			conn->snd_wscale = opt[2] > 14 ? 14 : opt[2];
			conn->peer_wscale_seen = 1;
		} else if (kind == 4 && opt[1] == 2) {
			conn->sack_permitted = 1;
		}

		left -= opt[1];
		opt += opt[1];
	}
}

static int tcp_rx_autotune(tcp_conn_t *conn, size_t incoming_len)
{
	if (!conn || !conn->rx_buf)
		return -EINVAL;

	if (incoming_len <= tcp_recv_window_full(conn))
		return 0;

	if (conn->rx_cap >= TCP_RCVBUF_MAX)
		return 0;

	size_t new_cap = conn->rx_cap ? conn->rx_cap : TCP_RCVBUF_MIN;
	while (new_cap < TCP_RCVBUF_MAX &&
		   incoming_len > new_cap - conn->rx_len) {
		new_cap *= 2;
		if (new_cap > TCP_RCVBUF_MAX)
			new_cap = TCP_RCVBUF_MAX;
	}

	if (new_cap <= conn->rx_cap)
		return 0;

	char *new_buf = kzalloc(new_cap);
	if (!new_buf)
		return -ENOMEM;

	size_t old_len = conn->rx_len;
	if (old_len)
		tcp_rx_pop(conn, new_buf, old_len);

	kfree(conn->rx_buf);
	conn->rx_buf = new_buf;
	conn->rx_cap = new_cap;
	conn->rx_head = 0;
	conn->rx_len = old_len;
	conn->rcv_wscale = tcp_wscale_for_cap(conn->rx_cap);

	return 0;
}

static int tcp_send_conn_packet(tcp_conn_t *conn, uint8_t flags,
								const void *payload, size_t payload_len)
{
	return net_send_ipv4_tcp_window(
		conn->dev, conn->peer_mac, conn->remote_ip, conn->local_port,
		conn->remote_port, conn->seq, conn->ack, flags, tcp_recv_window(conn),
		payload, payload_len);
}

static int tcp_send_ack(tcp_conn_t *conn)
{
	return tcp_send_conn_packet(conn, TCP_ACK, NULL, 0);
}

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
	tcp_parse_syn_options(conn, tcp, (tcp->data_off >> 4) * 4);
	conn->handler = listener->handler;
	conn->accept_handler = listener->accept_handler;
	conn->handler_ctx = listener->ctx;
	conn->passive = 1;

	if (conn->accept_handler) {
		conn->rx_cap = TCP_ACTIVE_RX_CAP;
		conn->rcv_wscale = tcp_wscale_for_cap(conn->rx_cap);
		conn->rx_buf = kzalloc(conn->rx_cap);
		if (!conn->rx_buf) {
			kfree(conn);
			tcp_send_reset(dev, src_mac, ip, tcp);
			return;
		}
	}
	conn->rcv_wscale = tcp_wscale_for_cap(conn->rx_cap);
	conn->heap_allocated = 1;
	tcp_conn_add(conn);

	net_send_ipv4_tcp_window_opts(dev, conn->peer_mac, conn->remote_ip,
								  conn->local_port, conn->remote_port, conn->seq,
								  conn->ack, TCP_SYN | TCP_ACK, tcp_recv_window(conn),
								  1, 1, conn->rcv_wscale, 1, NULL, 0);
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

		int r = net_send_ipv4_tcp_window(
			conn->dev, conn->peer_mac, conn->remote_ip, conn->local_port,
			conn->remote_port, seq, conn->ack, TCP_PSH | TCP_ACK,
			tcp_recv_window(conn), p, chunk);
		if (r != 0) {
			conn->seq = seq;
			return r;
		}

		p += chunk;
		len -= chunk;
	}

	return 0;
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
	if (r != 0)
		response_len = 0;

	if (response_len > TCP_PASSIVE_RESPONSE_CAP)
		response_len = TCP_PASSIVE_RESPONSE_CAP;

	if (response_len) {
		if (tcp_send_payload(conn, response, response_len) != 0)
			response_len = 0;
	}

	kfree(response);

	uint32_t fin_seq = conn->seq;
	conn->seq++;

	if (net_send_ipv4_tcp_window(conn->dev, conn->peer_mac, conn->remote_ip,
								 conn->local_port, conn->remote_port, fin_seq,
								 conn->ack, TCP_FIN | TCP_ACK,
								 tcp_recv_window(conn), NULL, 0) != 0) {
		conn->seq = fin_seq;
		return;
	}

	conn->closed = 1;
	sched_io_wake_all();
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
		sched_io_wake_all();
		if (conn->passive)
			tcp_close_passive(conn);
		return;
	}

	if (conn->passive) {
		if (!conn->connected && (flags & TCP_ACK) && ack == conn->seq + 1) {
			conn->seq++;
			conn->connected = 1;
			sched_io_wake_all();

			if (conn->accept_handler) {
				int r = conn->accept_handler(conn->dev, conn->remote_ip,
											 conn->remote_port, conn,
											 conn->handler_ctx);
				if (r != 0) {
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
				tcp_send_ack(conn);
				tcp_close_passive(conn);
			} else if (conn->closed && (flags & TCP_ACK)) {
				tcp_close_passive(conn);
			}
			return;
		}
	}

	if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
		ack == conn->seq + 1) {
		tcp_parse_syn_options(conn, tcp, tcp_hlen);
		conn->ack = seq + 1;
		conn->seq++;
		conn->connected = 1;
		sched_io_wake_all();
		tcp_send_ack(conn);
		return;
	}

	if (!conn->connected || seq != conn->ack)
		return;

	if (payload_len) {
		(void)tcp_rx_autotune(conn, payload_len);

		size_t free = tcp_recv_window_full(conn);

		if (payload_len <= free) {
			tcp_rx_push(conn, payload, payload_len);
			conn->ack += (uint32_t)payload_len;
			conn->rx_since_ack++;
			conn->rx_full_events = 0;

			log_debug(
				"tcp",
				"active queued payload: got=%zu queued=%zu cap=%zu wnd=%u",
				payload_len, conn->rx_len, conn->rx_cap, tcp_recv_window(conn));
			sched_io_wake_all();

			/* Delayed ACK heuristic: ACK every other received segment, and
			 * immediately when the advertised window is getting tight. recv() also
			 * sends a window-update ACK after userspace drains the buffer. */
			if (conn->rx_since_ack >= TCP_DELAYED_ACK_SEGMENTS ||
				tcp_recv_window_full(conn) < TCP_SEGMENT_DATA_MAX * 4) {
				conn->rx_since_ack = 0;
				tcp_send_ack(conn);
			}
		} else {
			conn->rx_full_events++;
			log_debug(
				"tcp",
				"active rx full: got=%zu free=%zu queued=%zu cap=%zu sack=%d; duplicate ack",
				payload_len, free, conn->rx_len, conn->rx_cap,
				conn->sack_permitted);

			/* No out-of-order queue exists yet, so this is still cumulative ACK
			 * only. Negotiating SACK-permitted is useful for peers, but emitting
			 * SACK blocks requires an out-of-order scoreboard. */
			tcp_send_ack(conn);
		}
	}

	if (flags & TCP_FIN) {
		conn->ack++;
		tcp_send_ack(conn);
		conn->closed = 1;
		sched_io_wake_all();
	}
}

int net_tcp_connect_ip(netdev_t *dev, uint32_t dst_ip, uint16_t port,
					   net_tcp_conn_t **out, uint64_t timeout_ms)
{
	if (!dev || !dst_ip || !port || !out)
		return -EINVAL;

	log_debug("tcp", "connect: dst_ip=%x port=%d timeout=%llu", dst_ip, port,
			  timeout_ms);

	uint32_t next_hop = dst_ip;
	if ((dst_ip & dev->ipv4_netmask) != (dev->ipv4_addr & dev->ipv4_netmask)) {
		if (!dev->ipv4_gateway) {
			log_debug("tcp", "connect: no gateway for off-link destination");
			return -ENOENT;
		}
		next_hop = dev->ipv4_gateway;
	}

	log_debug("tcp", "connect: dev=%s src=%u.%u.%u.%u next_hop=%x", dev->name,
			  (dev->ipv4_addr >> 24) & 0xff, (dev->ipv4_addr >> 16) & 0xff,
			  (dev->ipv4_addr >> 8) & 0xff, dev->ipv4_addr & 0xff, next_hop);

	tcp_conn_t *conn = kzalloc(sizeof(*conn));
	if (!conn)
		return -ENOMEM;

	conn->dev = dev;
	conn->remote_ip = dst_ip;
	conn->remote_port = port;
	conn->local_port = 40000 + (uint16_t)(pit_get_ticks() & 0x3fff);
	conn->seq = 0x4c595200u + (uint32_t)(pit_get_ticks() & 0xffff);
	conn->heap_allocated = 1;
	conn->rx_cap = TCP_ACTIVE_RX_CAP;
	conn->rcv_wscale = tcp_wscale_for_cap(conn->rx_cap);
	conn->rx_buf = kzalloc(conn->rx_cap);
	if (!conn->rx_buf) {
		kfree(conn);
		return -ENOMEM;
	}

	log_debug("tcp", "connect: calling arp_resolve");
	int r = net_arp_resolve(dev, next_hop, timeout_ms, conn->peer_mac);
	log_debug("tcp", "connect: arp_resolve returned %d", r);
	if (r != 0) {
		if (conn->rx_buf)
			kfree(conn->rx_buf);
		kfree(conn);
		return r;
	}

	tcp_conn_add(conn);

	log_debug("tcp", "connect: sending SYN");
	r = net_send_ipv4_tcp_window_opts(dev, conn->peer_mac, dst_ip,
									conn->local_port, port, conn->seq, 0, TCP_SYN,
									tcp_recv_window(conn), 1, 1, conn->rcv_wscale, 1,
									NULL, 0);
	if (r != 0) {
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
	while (!conn->connected && !conn->error && pit_get_ticks() < until) {
		if (tcp_interrupted() != 0) {
			tcp_conn_remove(conn);
			if (conn->rx_buf)
				kfree(conn->rx_buf);
			kfree(conn);
			return -EINTR;
		}

		net_poll_all();
		__asm__ volatile("pause" ::: "memory");
	}

	if (conn->error) {
		log_debug("tcp", "connect: reset by peer");
		tcp_conn_remove(conn);
		if (conn->rx_buf)
			kfree(conn->rx_buf);
		kfree(conn);
		return -ENOENT;
	}

	if (!conn->connected) {
		log_debug("tcp", "connect: timed out waiting for SYN-ACK");
		tcp_conn_remove(conn);
		if (conn->rx_buf)
			kfree(conn->rx_buf);
		kfree(conn);
		return -ETIMEDOUT;
	}

	log_debug("tcp", "connect: 3-way handshake complete");
	*out = conn;
	return 0;
}

int net_tcp_connect(const char *host, uint16_t port, net_tcp_conn_t **out,
					uint64_t timeout_ms)
{
	if (!host || !port || !out)
		return -EINVAL;

	netdev_t *dev = net_default_dev();
	if (!dev)
		return -ENOENT;

	uint32_t ip = 0;
	int r = net_dns_resolve_dev(dev, host, timeout_ms, &ip);
	if (r != 0)
		return r;

	return net_tcp_connect_ip(dev, ip, port, out, timeout_ms);
}

int net_tcp_send(net_tcp_conn_t *conn_, const void *buf, size_t len,
				 size_t *done, uint64_t timeout_ms)
{
	(void)timeout_ms;

	tcp_conn_t *conn = conn_;

	if (!conn || !buf)
		return -EINVAL;

	if (!conn->connected || conn->closed || conn->error)
		return -ENOENT;

	int r = tcp_send_payload(conn, buf, len);
	if (r != 0)
		return r;

	if (done)
		*done = len;

	return 0;
}

int net_tcp_recv(net_tcp_conn_t *conn_, void *buf, size_t cap, size_t *done,
				 uint64_t timeout_ms)
{
	tcp_conn_t *conn = conn_;

	if (!conn || !buf || !cap)
		return -EINVAL;

	if (done)
		*done = 0;

	if (!conn->connected)
		return -ENOENT;

	uint64_t until = pit_get_ticks() + net_timeout_ticks(timeout_ms);

	log_debug("tcp",
			  "recv wait: cap=%zu queued=%zu closed=%d error=%d timeout=%llu",
			  cap, conn->rx_len, conn->closed, conn->error, timeout_ms);

	while (!conn->rx_len && !conn->closed && !conn->error &&
		   pit_get_ticks() < until) {
		if (tcp_interrupted() != 0)
			return -EINTR;
		uint64_t now = pit_get_ticks();
		uint64_t remain_ms = timeout_ms;
		if (until > now && pit_get_hz() != 0)
			remain_ms = ((until - now) * 1000ULL) / pit_get_hz();
		time_timeout_t timeout;
		time_timeout_after_ms((int)remain_ms, &timeout);
		unsigned io_seq = sched_io_wait_prepare();
		net_poll_all();
		if (conn->rx_len || conn->closed || conn->error)
			break;
		int wr = sched_io_wait(io_seq, &timeout);
		if (wr == -EINTR)
			return -EINTR;
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

		tcp_rx_pop(conn, buf, copy);

		if (done)
			*done = copy;

		log_debug("tcp",
				  "recv: copied=%zu remaining=%zu closed=%d error=%d wnd=%u",
				  copy, conn->rx_len, conn->closed, conn->error,
				  tcp_recv_window(conn));

		if (!conn->closed && !conn->error) {
			conn->rx_since_ack = 0;
			tcp_send_ack(conn);
		}

		return 0;
	}

	if (conn->error) {
		log_debug("tcp", "recv: connection error with empty rx queue");
		return -ENOENT;
	}

	if (conn->closed) {
		log_debug("tcp", "recv: clean EOF");
		return 0;
	}

	log_debug("tcp", "recv: timeout/no data closed=%d error=%d", conn->closed,
			  conn->error);
	return -ETIMEDOUT;
}

void net_tcp_close(net_tcp_conn_t *conn_)
{
	tcp_conn_t *conn = conn_;

	if (!conn)
		return;

	if (conn->connected && !conn->closed && !conn->error) {
		uint32_t fin_seq = conn->seq;
		conn->seq++;

		net_send_ipv4_tcp_window(conn->dev, conn->peer_mac, conn->remote_ip,
								 conn->local_port, conn->remote_port, fin_seq,
								 conn->ack, TCP_FIN | TCP_ACK,
								 tcp_recv_window(conn), NULL, 0);
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
		return -EINVAL;

	uint32_t next_hop = dst_ip;
	if ((dst_ip & dev->ipv4_netmask) != (dev->ipv4_addr & dev->ipv4_netmask)) {
		if (!dev->ipv4_gateway)
			return -ENOENT;
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
	conn.rcv_wscale = tcp_wscale_for_cap(conn.rx_cap);

	int r = net_arp_resolve(dev, next_hop, timeout_ms, conn.peer_mac);
	if (r != 0)
		return r;

	tcp_conn_add(&conn);

	r = net_send_ipv4_tcp_window_opts(dev, conn.peer_mac, dst_ip,
									conn.local_port, 80, conn.seq, 0, TCP_SYN,
									tcp_recv_window(&conn), 1, 1, conn.rcv_wscale, 1,
									NULL, 0);
	if (r != 0)
		goto out;
	uint64_t until = pit_get_ticks() + net_timeout_ticks(timeout_ms);
	net_poll_until(dev, until, &conn.connected);
	if (!conn.connected || conn.error) {
		r = -ENOENT;
		goto out;
	}

	char req[512];
	int n = npf_snprintf(
		req, sizeof(req),
		"GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\nUser-Agent: lyr/0\r\n\r\n",
		path, host);
	if (n < 0 || (size_t)n >= sizeof(req)) {
		r = -EINVAL;
		goto out;
	}

	r = net_send_ipv4_tcp(dev, conn.peer_mac, dst_ip, conn.local_port, 80,
						  conn.seq, conn.ack, TCP_PSH | TCP_ACK, req,
						  (size_t)n);
	if (r != 0)
		goto out;
	conn.seq += (uint32_t)n;

	until = pit_get_ticks() + net_timeout_ticks(timeout_ms);
	net_poll_until(dev, until, &conn.closed);
	if (conn.error) {
		r = -ENOENT;
		goto out;
	}
	if (done)
		*done = conn.rx_len;
	r = conn.rx_len ? 0 : -ENOENT;

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
		return -EINVAL;

	if (tcp_listener_conflicts(local_ip, port))
		return -EEXIST;

	tcp_listener_t *listener = kzalloc(sizeof(*listener));
	if (!listener)
		return -ENOMEM;
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
	return 0;
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

static int tcp_ipv4_receive(const net_ipv4_rx_info_t *rx, void *ctx)
{
	(void)ctx;

	if (!rx || !rx->dev || !rx->src_mac || !rx->ip)
		return -EINVAL;

	net_tcp_receive(rx->dev, rx->src_mac, rx->ip, rx->ihl, rx->ip_len);
	return 1;
}

int net_tcp_init(void)
{
	return net_ipv4_register_protocol(&tcp_ipv4_protocol);
}
