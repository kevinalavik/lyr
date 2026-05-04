#include "internal.h"
#include <dev/pit.h>
#include <fs/vfs.h>
#include <lib/string.h>

#define DNS_PORT 53

typedef struct __attribute__((packed)) {
	uint16_t id;
	uint16_t flags;
	uint16_t qdcount;
	uint16_t ancount;
	uint16_t nscount;
	uint16_t arcount;
} dns_hdr_t;

static uint16_t dns_query_id;
static uint16_t dns_query_port;
static uint32_t dns_result_ip;
static int dns_ready;

static int dns_encode_name(const char *name, uint8_t *out, size_t cap,
						   size_t *done)
{
	size_t pos = 0;
	size_t label_len = 0;
	size_t label_pos = 0;

	if (!name || !name[0] || !out || !done)
		return VFS_ERR_INVAL;

	while (*name) {
		if (label_len == 0) {
			if (pos >= cap)
				return VFS_ERR_INVAL;
			label_pos = pos++;
		}

		if (*name == '.') {
			if (label_len == 0 || label_len > 63)
				return VFS_ERR_INVAL;
			out[label_pos] = (uint8_t)label_len;
			label_len = 0;
			name++;
			continue;
		}

		if (pos >= cap)
			return VFS_ERR_INVAL;
		out[pos++] = (uint8_t)*name++;
		label_len++;
		if (label_len > 63)
			return VFS_ERR_INVAL;
	}

	if (label_len == 0)
		return VFS_ERR_INVAL;
	out[label_pos] = (uint8_t)label_len;
	if (pos >= cap)
		return VFS_ERR_INVAL;
	out[pos++] = 0;
	*done = pos;
	return VFS_OK;
}

static int dns_skip_name(const uint8_t *msg, size_t len, size_t *pos)
{
	size_t p;
	if (!msg || !pos)
		return VFS_ERR_INVAL;
	p = *pos;

	for (;;) {
		if (p >= len)
			return VFS_ERR_INVAL;
		uint8_t n = msg[p++];
		if (n == 0)
			break;
		if ((n & 0xc0) == 0xc0) {
			if (p >= len)
				return VFS_ERR_INVAL;
			p++;
			break;
		}
		if (n & 0xc0)
			return VFS_ERR_INVAL;
		if (p + n > len)
			return VFS_ERR_INVAL;
		p += n;
	}

	*pos = p;
	return VFS_OK;
}

static int send_dns_query(netdev_t *dev, uint32_t server_ip, const char *name)
{
	uint8_t query[512];
	dns_hdr_t *hdr = (dns_hdr_t *)query;
	size_t pos = sizeof(*hdr);
	size_t name_len = 0;

	memset(query, 0, sizeof(query));
	hdr->id = htons(dns_query_id);
	hdr->flags = htons(0x0100);
	hdr->qdcount = htons(1);

	int r = dns_encode_name(name, query + pos, sizeof(query) - pos, &name_len);
	if (r != VFS_OK)
		return r;
	pos += name_len;
	if (pos + 4 > sizeof(query))
		return VFS_ERR_INVAL;
	query[pos++] = 0;
	query[pos++] = 1;
	query[pos++] = 0;
	query[pos++] = 1;

	uint8_t mac[NET_ETH_ALEN];
	uint32_t next_hop = server_ip;
	if ((server_ip & dev->ipv4_netmask) != (dev->ipv4_addr & dev->ipv4_netmask))
		next_hop = dev->ipv4_gateway;

	r = net_arp_resolve(dev, next_hop, 1000, mac);
	if (r != VFS_OK)
		return r;

	return net_send_ipv4_udp(dev, mac, dev->ipv4_addr, server_ip,
							 dns_query_port, DNS_PORT, query, pos);
}

void net_dns_receive(const udp_hdr_t *udp, size_t udp_len)
{
	if (ntohs(udp->src_port) != DNS_PORT ||
		ntohs(udp->dst_port) != dns_query_port)
		return;
	if (udp_len < sizeof(*udp) + sizeof(dns_hdr_t))
		return;

	const uint8_t *msg = (const uint8_t *)udp + sizeof(*udp);
	size_t len = udp_len - sizeof(*udp);
	const dns_hdr_t *hdr = (const dns_hdr_t *)msg;
	if (ntohs(hdr->id) != dns_query_id || !(ntohs(hdr->flags) & 0x8000))
		return;

	size_t pos = sizeof(*hdr);
	uint16_t qd = ntohs(hdr->qdcount);
	uint16_t an = ntohs(hdr->ancount);
	for (uint16_t i = 0; i < qd; i++) {
		if (dns_skip_name(msg, len, &pos) != VFS_OK || pos + 4 > len)
			return;
		pos += 4;
	}

	for (uint16_t i = 0; i < an; i++) {
		if (dns_skip_name(msg, len, &pos) != VFS_OK || pos + 10 > len)
			return;
		uint16_t type = ((uint16_t)msg[pos] << 8) | msg[pos + 1];
		uint16_t klass = ((uint16_t)msg[pos + 2] << 8) | msg[pos + 3];
		uint16_t rdlen = ((uint16_t)msg[pos + 8] << 8) | msg[pos + 9];
		pos += 10;
		if (pos + rdlen > len)
			return;
		if (type == 1 && klass == 1 && rdlen == 4) {
			dns_result_ip = ((uint32_t)msg[pos] << 24) |
							((uint32_t)msg[pos + 1] << 16) |
							((uint32_t)msg[pos + 2] << 8) | msg[pos + 3];
			dns_ready = 1;
			return;
		}
		pos += rdlen;
	}
}

int net_dns_resolve(const char *name, uint64_t timeout_ms, uint32_t *out_ip)
{
	netdev_t *dev = net_default_dev();
	if (!dev || !name || !out_ip)
		return VFS_ERR_INVAL;
	if (!dev->dns_server)
		return VFS_ERR_NOENT;

	dns_query_id++;
	if (!dns_query_id)
		dns_query_id = 1;
	dns_query_port = (uint16_t)(49152 + (dns_query_id & 0x3fff));
	dns_result_ip = 0;
	dns_ready = 0;

	int r = send_dns_query(dev, dev->dns_server, name);
	if (r != VFS_OK)
		return r;

	uint64_t until = pit_get_ticks() + net_timeout_ticks(timeout_ms);
	net_poll_until(dev, until, &dns_ready);
	if (!dns_ready || !dns_result_ip)
		return VFS_ERR_NOENT;

	*out_ip = dns_result_ip;
	return VFS_OK;
}
