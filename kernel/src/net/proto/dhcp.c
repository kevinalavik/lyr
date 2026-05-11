#include "../stack.h"
#include <debug/log.h>
#include <dev/pit.h>
#include <fs/vfs.h>
#include <lib/string.h>

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC 0x63825363u
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5

typedef struct __attribute__((packed)) {
	uint8_t op;
	uint8_t htype;
	uint8_t hlen;
	uint8_t hops;
	uint32_t xid;
	uint16_t secs;
	uint16_t flags;
	uint32_t ciaddr;
	uint32_t yiaddr;
	uint32_t siaddr;
	uint32_t giaddr;
	uint8_t chaddr[16];
	uint8_t sname[64];
	uint8_t file[128];
	uint32_t magic;
	uint8_t options[312];
} dhcp_pkt_t;

static uint32_t dhcp_xid;
static uint32_t dhcp_offered_ip;
static uint32_t dhcp_server_ip;
static uint32_t dhcp_netmask;
static uint32_t dhcp_gateway;
static uint32_t dhcp_dns_server;
static uint8_t dhcp_seen_type;
static int dhcp_ready;

static int dhcp_udp_receive(const net_udp_dgram_t *dgram, void *ctx);

static const net_udp_handler_ops_t dhcp_udp_handler = {
	.name = "dhcp",
	.receive = dhcp_udp_receive,
	.ctx = NULL,
};

static size_t dhcp_add_u8(uint8_t *opts, size_t pos, uint8_t code, uint8_t val)
{
	opts[pos++] = code;
	opts[pos++] = 1;
	opts[pos++] = val;
	return pos;
}

static size_t dhcp_add_u32(uint8_t *opts, size_t pos, uint8_t code,
						   uint32_t val)
{
	opts[pos++] = code;
	opts[pos++] = 4;
	opts[pos++] = (uint8_t)(val >> 24);
	opts[pos++] = (uint8_t)(val >> 16);
	opts[pos++] = (uint8_t)(val >> 8);
	opts[pos++] = (uint8_t)val;
	return pos;
}

static int send_dhcp(netdev_t *dev, uint8_t type, uint32_t requested_ip,
					 uint32_t server_ip)
{
	dhcp_pkt_t pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.op = 1;
	pkt.htype = 1;
	pkt.hlen = NET_ETH_ALEN;
	pkt.xid = htonl(dhcp_xid);
	pkt.flags = htons(0x8000);
	memcpy(pkt.chaddr, dev->mac, NET_ETH_ALEN);
	pkt.magic = htonl(DHCP_MAGIC);

	size_t pos = 0;
	pos = dhcp_add_u8(pkt.options, pos, 53, type);
	if (requested_ip)
		pos = dhcp_add_u32(pkt.options, pos, 50, requested_ip);
	if (server_ip)
		pos = dhcp_add_u32(pkt.options, pos, 54, server_ip);
	pkt.options[pos++] = 55;
	pkt.options[pos++] = 4;
	pkt.options[pos++] = 1;
	pkt.options[pos++] = 3;
	pkt.options[pos++] = 6;
	pkt.options[pos++] = 51;
	pkt.options[pos++] = 255;

	uint8_t bcast[NET_ETH_ALEN];
	memset(bcast, 0xff, sizeof(bcast));
	return net_send_ipv4_udp(dev, bcast, 0, 0xffffffffu, DHCP_CLIENT_PORT,
							 DHCP_SERVER_PORT, &pkt,
							 sizeof(pkt) - sizeof(pkt.options) + pos);
}

static int dhcp_udp_receive(const net_udp_dgram_t *dgram, void *ctx)
{
	(void)ctx;

	if (!dgram || !dgram->ipv4 || !dgram->ipv4->dev || !dgram->udp)
		return -EINVAL;
	if (dgram->udp_len < sizeof(*dgram->udp) + sizeof(dhcp_pkt_t) - 312)
		return 0;
	if (dgram->src_port != DHCP_SERVER_PORT ||
		dgram->dst_port != DHCP_CLIENT_PORT)
		return 0;

	netdev_t *dev = dgram->ipv4->dev;
	const dhcp_pkt_t *pkt = (const dhcp_pkt_t *)dgram->payload;
	size_t pkt_len = dgram->payload_len;
	if (pkt_len < sizeof(*pkt) - sizeof(pkt->options) ||
		ntohl(pkt->xid) != dhcp_xid || ntohl(pkt->magic) != DHCP_MAGIC ||
		pkt->op != 2 || memcmp(pkt->chaddr, dev->mac, NET_ETH_ALEN) != 0)
		return 0;

	size_t opt_len = pkt_len - (sizeof(*pkt) - sizeof(pkt->options));
	size_t pos = 0;
	uint8_t type = 0;
	uint32_t server = 0;
	uint32_t mask = 0;
	uint32_t gateway = 0;
	uint32_t dns = 0;

	while (pos < opt_len) {
		uint8_t code = pkt->options[pos++];
		if (code == 0)
			continue;
		if (code == 255)
			break;
		if (pos >= opt_len)
			break;
		uint8_t len = pkt->options[pos++];
		if (pos + len > opt_len)
			break;
		const uint8_t *v = &pkt->options[pos];
		if (code == 53 && len >= 1)
			type = v[0];
		else if (code == 54 && len >= 4)
			server = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
					 ((uint32_t)v[2] << 8) | v[3];
		else if (code == 1 && len >= 4)
			mask = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
				   ((uint32_t)v[2] << 8) | v[3];
		else if (code == 3 && len >= 4)
			gateway = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
					  ((uint32_t)v[2] << 8) | v[3];
		else if (code == 6 && len >= 4)
			dns = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
				  ((uint32_t)v[2] << 8) | v[3];
		pos += len;
	}

	if (type != DHCP_OFFER && type != DHCP_ACK)
		return 0;

	dhcp_offered_ip = ntohl(pkt->yiaddr);
	dhcp_server_ip = server;
	dhcp_netmask = mask;
	dhcp_gateway = gateway;
	dhcp_dns_server = dns;
	dhcp_seen_type = type;
	dhcp_ready = 1;
	return 1;
}

int net_dhcp_configure_dev(netdev_t *dev, uint64_t timeout_ms)
{
	if (!dev)
		return -ENOENT;

	dhcp_xid = 0x4c595244u;
	dhcp_ready = 0;
	dhcp_seen_type = 0;
	dhcp_offered_ip = 0;
	dhcp_server_ip = 0;
	dhcp_netmask = 0;
	dhcp_gateway = 0;
	dhcp_dns_server = 0;

	uint64_t until = pit_get_ticks() + net_timeout_ticks(timeout_ms);
	int r = send_dhcp(dev, DHCP_DISCOVER, 0, 0);
	if (r != 0)
		return r;
	net_poll_until(dev, until, &dhcp_ready);
	if (!dhcp_ready || dhcp_seen_type != DHCP_OFFER || !dhcp_offered_ip)
		return -ENOENT;

	uint32_t offered = dhcp_offered_ip;
	uint32_t server = dhcp_server_ip;
	dhcp_ready = 0;
	dhcp_seen_type = 0;
	until = pit_get_ticks() + net_timeout_ticks(timeout_ms);
	r = send_dhcp(dev, DHCP_REQUEST, offered, server);
	if (r != 0)
		return r;
	net_poll_until(dev, until, &dhcp_ready);
	if (!dhcp_ready || dhcp_seen_type != DHCP_ACK)
		return -ENOENT;

	dev->ipv4_addr = dhcp_offered_ip;
	dev->dhcp_server = dhcp_server_ip;
	dev->ipv4_netmask = dhcp_netmask ? dhcp_netmask : net_ipv4(255, 255, 255, 0);
	dev->ipv4_gateway = dhcp_gateway ? dhcp_gateway : dhcp_server_ip;
	dev->dns_server = dhcp_dns_server ? dhcp_dns_server : dev->ipv4_gateway;

	dev->dhcp_configured = 1;

	char ip[24];
	char gw[24];
	char srv[24];
	char dns[24];
	net_ipv4_format(dev->ipv4_addr, ip, sizeof(ip));
	net_ipv4_format(dev->ipv4_gateway, gw, sizeof(gw));
	net_ipv4_format(dev->dhcp_server, srv, sizeof(srv));
	net_ipv4_format(dev->dns_server, dns, sizeof(dns));
	log_info("net", "%s DHCP lease ip=%s gateway=%s server=%s dns=%s",
			 dev->name, ip, gw, srv, dns);
	return 0;
}

int net_dhcp_configure(uint64_t timeout_ms)
{
	return net_dhcp_configure_dev(net_default_dev(), timeout_ms);
}

int net_dhcp_init(void)
{
	return net_udp_register_handler(&dhcp_udp_handler);
}
