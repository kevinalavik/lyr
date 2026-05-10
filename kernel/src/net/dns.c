#include "internal.h"
#include <dev/pit.h>
#include <fs/vfs.h>
#include <mm/heap.h>
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
static netdev_t *dns_dev;
static int dns_ready;

static int ascii_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int ascii_digit(char c)
{
	return c >= '0' && c <= '9';
}

static int host_token_eq(const char *token, size_t token_len, const char *name)
{
	return strlen(name) == token_len && memcmp(token, name, token_len) == 0;
}

static int parse_ipv4_literal(const char *s, size_t len, uint32_t *out_ip)
{
	if (!s || !out_ip || len == 0)
		return -EINVAL;

	uint32_t parts[4];
	size_t pos = 0;
	for (size_t part = 0; part < 4; part++) {
		if (pos >= len || !ascii_digit(s[pos]))
			return -EINVAL;
		uint32_t value = 0;
		size_t digits = 0;
		while (pos < len && ascii_digit(s[pos])) {
			value = value * 10 + (uint32_t)(s[pos] - '0');
			if (value > 255)
				return -EINVAL;
			pos++;
			digits++;
		}
		if (!digits)
			return -EINVAL;
		parts[part] = value;
		if (part < 3) {
			if (pos >= len || s[pos] != '.')
				return -EINVAL;
			pos++;
		}
	}
	if (pos != len)
		return -EINVAL;

	*out_ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) |
			  parts[3];
	return 0;
}

static int hosts_lookup(const char *name, uint32_t *out_ip)
{
	if (!name || !out_ip)
		return -EINVAL;

	vfs_file_t *file = NULL;
	int r = vfs_open("/etc/hosts", VFS_O_RDONLY, 0, &vfs_root_cred, &file);
	if (r != 0)
		return r;

	size_t cap = file->node->size;
	if (cap == 0 || VFS_S_ISCHR(file->node->mode))
		cap = 4096;
	char *buf = kzalloc(cap + 1);
	if (!buf) {
		vfs_close(file);
		return -ENOMEM;
	}

	size_t done = 0;
	r = vfs_read(file, buf, cap, &done);
	vfs_close(file);
	if (r != 0) {
		kfree(buf);
		return r;
	}
	buf[done] = '\0';

	size_t pos = 0;
	while (pos < done) {
		while (pos < done && (buf[pos] == '\n' || buf[pos] == '\r'))
			pos++;
		size_t line = pos;
		while (pos < done && buf[pos] != '\n' && buf[pos] != '\r')
			pos++;
		size_t line_end = pos;

		size_t hash = line;
		while (hash < line_end && buf[hash] != '#')
			hash++;
		line_end = hash;

		size_t p = line;
		while (p < line_end && ascii_space(buf[p]))
			p++;
		size_t ip_start = p;
		while (p < line_end && !ascii_space(buf[p]))
			p++;
		size_t ip_len = p - ip_start;
		if (!ip_len)
			continue;

		uint32_t ip = 0;
		if (parse_ipv4_literal(buf + ip_start, ip_len, &ip) != 0)
			continue;

		while (p < line_end) {
			while (p < line_end && ascii_space(buf[p]))
				p++;
			size_t host_start = p;
			while (p < line_end && !ascii_space(buf[p]))
				p++;
			size_t host_len = p - host_start;
			if (host_len && host_token_eq(buf + host_start, host_len, name)) {
				*out_ip = ip;
				kfree(buf);
				return 0;
			}
		}
	}

	kfree(buf);
	return -ENOENT;
}

static int dns_encode_name(const char *name, uint8_t *out, size_t cap,
						   size_t *done)
{
	size_t pos = 0;
	size_t label_len = 0;
	size_t label_pos = 0;

	if (!name || !name[0] || !out || !done)
		return -EINVAL;

	while (*name) {
		if (label_len == 0) {
			if (pos >= cap)
				return -EINVAL;
			label_pos = pos++;
		}

		if (*name == '.') {
			if (label_len == 0 || label_len > 63)
				return -EINVAL;
			out[label_pos] = (uint8_t)label_len;
			label_len = 0;
			name++;
			continue;
		}

		if (pos >= cap)
			return -EINVAL;
		out[pos++] = (uint8_t)*name++;
		label_len++;
		if (label_len > 63)
			return -EINVAL;
	}

	if (label_len == 0)
		return -EINVAL;
	out[label_pos] = (uint8_t)label_len;
	if (pos >= cap)
		return -EINVAL;
	out[pos++] = 0;
	*done = pos;
	return 0;
}

static int dns_skip_name(const uint8_t *msg, size_t len, size_t *pos)
{
	size_t p;
	if (!msg || !pos)
		return -EINVAL;
	p = *pos;

	for (;;) {
		if (p >= len)
			return -EINVAL;
		uint8_t n = msg[p++];
		if (n == 0)
			break;
		if ((n & 0xc0) == 0xc0) {
			if (p >= len)
				return -EINVAL;
			p++;
			break;
		}
		if (n & 0xc0)
			return -EINVAL;
		if (p + n > len)
			return -EINVAL;
		p += n;
	}

	*pos = p;
	return 0;
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
	if (r != 0)
		return r;
	pos += name_len;
	if (pos + 4 > sizeof(query))
		return -EINVAL;
	query[pos++] = 0;
	query[pos++] = 1;
	query[pos++] = 0;
	query[pos++] = 1;

	uint8_t mac[NET_ETH_ALEN];
	uint32_t next_hop = server_ip;
	if ((server_ip & dev->ipv4_netmask) != (dev->ipv4_addr & dev->ipv4_netmask))
		next_hop = dev->ipv4_gateway;

	r = net_arp_resolve(dev, next_hop, 1000, mac);
	if (r != 0)
		return r;

	return net_send_ipv4_udp(dev, mac, dev->ipv4_addr, server_ip,
							 dns_query_port, DNS_PORT, query, pos);
}

void net_dns_receive(netdev_t *dev, const udp_hdr_t *udp, size_t udp_len)
{
	if (dev != dns_dev)
		return;
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
		if (dns_skip_name(msg, len, &pos) != 0 || pos + 4 > len)
			return;
		pos += 4;
	}

	for (uint16_t i = 0; i < an; i++) {
		if (dns_skip_name(msg, len, &pos) != 0 || pos + 10 > len)
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

int net_dns_resolve_dev(netdev_t *dev, const char *name, uint64_t timeout_ms,
						uint32_t *out_ip)
{
	if (!dev || !name || !out_ip)
		return -EINVAL;

	int r = parse_ipv4_literal(name, strlen(name), out_ip);
	if (r == 0)
		return 0;

	r = hosts_lookup(name, out_ip);
	if (r == 0)
		return 0;

	if (!dev->dns_server)
		return -ENOENT;

	dns_query_id++;
	if (!dns_query_id)
		dns_query_id = 1;
	dns_query_port = (uint16_t)(49152 + (dns_query_id & 0x3fff));
	dns_result_ip = 0;
	dns_dev = dev;
	dns_ready = 0;

	r = send_dns_query(dev, dev->dns_server, name);
	if (r != 0)
		return r;

	uint64_t until = pit_get_ticks() + net_timeout_ticks(timeout_ms);
	net_poll_until(dev, until, &dns_ready);
	if (!dns_ready || !dns_result_ip)
		return -ENOENT;

	*out_ip = dns_result_ip;
	return 0;
}

int net_dns_resolve(const char *name, uint64_t timeout_ms, uint32_t *out_ip)
{
	netdev_t *dev = net_default_dev();
	if (!dev)
		return -ENOENT;
	return net_dns_resolve_dev(dev, name, timeout_ms, out_ip);
}
