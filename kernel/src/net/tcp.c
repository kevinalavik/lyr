#include "internal.h"
#include <dev/pit.h>
#include <fs/vfs.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>

static uint32_t tcp_remote_ip;
static uint16_t tcp_remote_port;
static uint16_t tcp_local_port;
static uint32_t tcp_seq;
static uint32_t tcp_ack;
static uint8_t tcp_peer_mac[NET_ETH_ALEN];
static char *tcp_rx_buf;
static size_t tcp_rx_cap;
static size_t tcp_rx_len;
static int tcp_connected;
static int tcp_closed;
static int tcp_error;

void net_tcp_receive(netdev_t *dev, const ipv4_hdr_t *ip, size_t ihl,
					 size_t ip_len)
{
	if (ip_len < ihl + sizeof(tcp_hdr_t))
		return;

	const tcp_hdr_t *tcp = (const tcp_hdr_t *)((const uint8_t *)ip + ihl);
	if (ntohs(tcp->src_port) != tcp_remote_port ||
		ntohs(tcp->dst_port) != tcp_local_port ||
		ntohl(ip->src) != tcp_remote_ip)
		return;

	size_t tcp_hlen = (tcp->data_off >> 4) * 4;
	if (tcp_hlen < sizeof(*tcp) || ip_len < ihl + tcp_hlen)
		return;

	uint8_t flags = tcp->flags;
	uint32_t seq = ntohl(tcp->seq);
	uint32_t ack = ntohl(tcp->ack);
	size_t payload_len = ip_len - ihl - tcp_hlen;
	const uint8_t *payload = (const uint8_t *)tcp + tcp_hlen;

	if (flags & 0x04) {
		tcp_error = 1;
		tcp_closed = 1;
		return;
	}

	if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
		ack == tcp_seq + 1) {
		tcp_ack = seq + 1;
		tcp_seq++;
		tcp_connected = 1;
		net_send_ipv4_tcp(dev, tcp_peer_mac, tcp_remote_ip, tcp_local_port,
						  tcp_remote_port, tcp_seq, tcp_ack, TCP_ACK, NULL, 0);
		return;
	}

	if (!tcp_connected || seq != tcp_ack)
		return;

	if (payload_len) {
		size_t copy = payload_len;
		if (copy > tcp_rx_cap - tcp_rx_len)
			copy = tcp_rx_cap - tcp_rx_len;
		if (copy) {
			memcpy(tcp_rx_buf + tcp_rx_len, payload, copy);
			tcp_rx_len += copy;
		}
		tcp_ack += (uint32_t)payload_len;
		net_send_ipv4_tcp(dev, tcp_peer_mac, tcp_remote_ip, tcp_local_port,
						  tcp_remote_port, tcp_seq, tcp_ack, TCP_ACK, NULL, 0);
		if (tcp_rx_len == tcp_rx_cap)
			tcp_closed = 1;
	}

	if (flags & TCP_FIN) {
		tcp_ack++;
		net_send_ipv4_tcp(dev, tcp_peer_mac, tcp_remote_ip, tcp_local_port,
						  tcp_remote_port, tcp_seq, tcp_ack, TCP_ACK, NULL, 0);
		tcp_closed = 1;
	}
}

int net_tcp_http_request(netdev_t *dev, uint32_t dst_ip, const char *host,
						 const char *path, char *buf, size_t len,
						 size_t *done, uint64_t timeout_ms)
{
	if (!dev || !host || !path || !buf || len == 0)
		return VFS_ERR_INVAL;

	uint32_t next_hop = dst_ip;
	if ((dst_ip & dev->ipv4_netmask) != (dev->ipv4_addr & dev->ipv4_netmask))
		next_hop = dev->ipv4_gateway;

	int r = net_arp_resolve(dev, next_hop, timeout_ms, tcp_peer_mac);
	if (r != VFS_OK)
		return r;

	tcp_remote_ip = dst_ip;
	tcp_remote_port = 80;
	tcp_local_port = 40000 + (uint16_t)(pit_get_ticks() & 0x3fff);
	tcp_seq = 0x4c595200u + (uint32_t)(pit_get_ticks() & 0xffff);
	tcp_ack = 0;
	tcp_rx_buf = buf;
	tcp_rx_cap = len;
	tcp_rx_len = 0;
	tcp_connected = 0;
	tcp_closed = 0;
	tcp_error = 0;

	r = net_send_ipv4_tcp(dev, tcp_peer_mac, dst_ip, tcp_local_port, 80,
						  tcp_seq, 0, TCP_SYN, NULL, 0);
	if (r != VFS_OK)
		return r;
	uint64_t until = pit_get_ticks() + net_timeout_ticks(timeout_ms);
	net_poll_until(dev, until, &tcp_connected);
	if (!tcp_connected || tcp_error)
		return VFS_ERR_NOENT;

	char req[512];
	int n = npf_snprintf(req, sizeof(req),
						 "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\nUser-Agent: lyr/0\r\n\r\n",
						 path, host);
	if (n < 0 || (size_t)n >= sizeof(req))
		return VFS_ERR_INVAL;

	r = net_send_ipv4_tcp(dev, tcp_peer_mac, dst_ip, tcp_local_port, 80,
						  tcp_seq, tcp_ack, TCP_PSH | TCP_ACK, req,
						  (size_t)n);
	if (r != VFS_OK)
		return r;
	tcp_seq += (uint32_t)n;

	until = pit_get_ticks() + net_timeout_ticks(timeout_ms);
	net_poll_until(dev, until, &tcp_closed);
	if (tcp_error)
		return VFS_ERR_NOENT;
	if (done)
		*done = tcp_rx_len;
	return tcp_rx_len ? VFS_OK : VFS_ERR_NOENT;
}
