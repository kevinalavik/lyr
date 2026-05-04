#include <net/net.h>
#include <debug/log.h>
#include <dev/pit.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <sync/spinlock.h>

#define ETH_P_IP 0x0800
#define ETH_P_ARP 0x0806
#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP 17
#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO_REQUEST 8
#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC 0x63825363u
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5

typedef struct __attribute__((packed)) {
	uint8_t dst[6];
	uint8_t src[6];
	uint16_t type;
} eth_hdr_t;

typedef struct __attribute__((packed)) {
	uint16_t htype;
	uint16_t ptype;
	uint8_t hlen;
	uint8_t plen;
	uint16_t oper;
	uint8_t sha[6];
	uint32_t spa;
	uint8_t tha[6];
	uint32_t tpa;
} arp_pkt_t;

typedef struct __attribute__((packed)) {
	uint8_t ver_ihl;
	uint8_t tos;
	uint16_t total_len;
	uint16_t id;
	uint16_t frag_off;
	uint8_t ttl;
	uint8_t proto;
	uint16_t checksum;
	uint32_t src;
	uint32_t dst;
} ipv4_hdr_t;

typedef struct __attribute__((packed)) {
	uint8_t type;
	uint8_t code;
	uint16_t checksum;
	uint16_t ident;
	uint16_t seq;
	uint8_t payload[32];
} icmp_echo_t;

typedef struct __attribute__((packed)) {
	uint16_t src_port;
	uint16_t dst_port;
	uint16_t len;
	uint16_t checksum;
} udp_hdr_t;

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

static spinlock_t net_lock = SPINLOCK_INIT;
static netdev_t *netdevs;
static uint8_t arp_mac[6];
static uint32_t arp_ip;
static int arp_ready;
static int ping_ready;
static uint16_t ping_ident;
static uint16_t ping_seq;
static uint32_t ping_src;
static uint64_t ping_started_tick;
static net_ping_result_t ping_result;
static uint32_t dhcp_xid;
static uint32_t dhcp_offered_ip;
static uint32_t dhcp_server_ip;
static uint32_t dhcp_netmask;
static uint32_t dhcp_gateway;
static uint8_t dhcp_seen_type;
static int dhcp_ready;

static uint16_t bswap16(uint16_t v)
{
	return (uint16_t)((v << 8) | (v >> 8));
}

static uint32_t bswap32(uint32_t v)
{
	return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
		   ((v & 0x00ff0000u) >> 8) | ((v & 0xff000000u) >> 24);
}

#define htons(v) bswap16((uint16_t)(v))
#define ntohs(v) bswap16((uint16_t)(v))
#define htonl(v) bswap32((uint32_t)(v))
#define ntohl(v) bswap32((uint32_t)(v))

uint32_t net_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

uint32_t net_default_ipv4(void)
{
	return netdevs ? netdevs->ipv4_addr : 0;
}

uint32_t net_default_gateway(void)
{
	return netdevs ? netdevs->ipv4_gateway : 0;
}

static uint16_t checksum(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t sum = 0;
	while (len > 1) {
		sum += ((uint16_t)p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}
	if (len)
		sum += ((uint16_t)p[0] << 8);
	while (sum >> 16)
		sum = (sum & 0xffffu) + (sum >> 16);
	return (uint16_t)~sum;
}

void net_ipv4_format(uint32_t ip, char *out, size_t len)
{
	npf_snprintf(out, len, "%u.%u.%u.%u", (ip >> 24) & 0xff,
				 (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
}

static int netdevs_read(void *ctx, uint64_t off, void *buf, size_t len,
						size_t *done)
{
	(void)ctx;
	if (done)
		*done = 0;
	if (!buf)
		return VFS_ERR_INVAL;

	char tmp[1024];
	size_t pos = 0;
	for (netdev_t *dev = netdevs; dev && pos < sizeof(tmp); dev = dev->next) {
		char ip[24];
		char gw[24];
		net_ipv4_format(dev->ipv4_addr, ip, sizeof(ip));
		net_ipv4_format(dev->ipv4_gateway, gw, sizeof(gw));
		int n = npf_snprintf(tmp + pos, sizeof(tmp) - pos,
							 "%s mac=%02x:%02x:%02x:%02x:%02x:%02x ip=%s gateway=%s mtu=%u dhcp=%s\n",
							 dev->name, dev->mac[0], dev->mac[1],
							 dev->mac[2], dev->mac[3], dev->mac[4],
							 dev->mac[5], ip, gw, dev->mtu,
							 dev->dhcp_configured ? "yes" : "no");
		if (n < 0)
			break;
		if ((size_t)n >= sizeof(tmp) - pos) {
			pos = sizeof(tmp) - 1;
			break;
		}
		pos += (size_t)n;
	}

	if (off >= pos || len == 0)
		return VFS_OK;
	size_t copy = pos - (size_t)off;
	if (copy > len)
		copy = len;
	memcpy(buf, tmp + off, copy);
	if (done)
		*done = copy;
	return VFS_OK;
}

int net_init(void)
{
	int r = devfs_mkdir("/dev/net", 0755);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;
	r = devfs_register_chr("/dev/net/devices", 0444, netdevs_read, NULL, NULL);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;
	log_info("net", "network core ok");
	return VFS_OK;
}

int netdev_register(netdev_t *src)
{
	if (!src || !src->send || !src->poll || !src->name[0])
		return VFS_ERR_INVAL;

	netdev_t *dev = kzalloc(sizeof(*dev));
	if (!dev)
		return VFS_ERR_NOMEM;
	memcpy(dev, src, sizeof(*dev));
	if (!dev->mtu)
		dev->mtu = NET_MTU;

	spinlock_acquire(&net_lock);
	dev->next = netdevs;
	netdevs = dev;
	spinlock_release(&net_lock);

	log_info("net", "%s registered mac=%02x:%02x:%02x:%02x:%02x:%02x",
			 dev->name, dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3],
			 dev->mac[4], dev->mac[5]);

	int r = net_dhcp_configure(2000);
	if (r != VFS_OK) {
		if (!dev->ipv4_addr)
			dev->ipv4_addr = net_ipv4(10, 0, 2, 15);
		if (!dev->ipv4_gateway)
			dev->ipv4_gateway = net_ipv4(10, 0, 2, 2);
		if (!dev->ipv4_netmask)
			dev->ipv4_netmask = net_ipv4(255, 255, 255, 0);
		log_warn("net", "%s DHCP failed status=%d, using %u.%u.%u.%u",
				 dev->name, r, (dev->ipv4_addr >> 24) & 0xff,
				 (dev->ipv4_addr >> 16) & 0xff,
				 (dev->ipv4_addr >> 8) & 0xff, dev->ipv4_addr & 0xff);
	}
	return VFS_OK;
}

static int send_ipv4_udp(netdev_t *dev, const uint8_t dst_mac[6],
						 uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
						 uint16_t dst_port, const void *payload,
						 size_t payload_len)
{
	if (!dev || !payload || payload_len > 1400)
		return VFS_ERR_INVAL;

	uint8_t frame[sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) +
				  sizeof(dhcp_pkt_t)];
	size_t udp_len = sizeof(udp_hdr_t) + payload_len;
	size_t ip_len = sizeof(ipv4_hdr_t) + udp_len;
	size_t frame_len = sizeof(eth_hdr_t) + ip_len;
	eth_hdr_t *eth = (eth_hdr_t *)frame;
	ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(*eth));
	udp_hdr_t *udp = (udp_hdr_t *)((uint8_t *)ip + sizeof(*ip));
	uint8_t *body = (uint8_t *)udp + sizeof(*udp);

	memcpy(eth->dst, dst_mac, 6);
	memcpy(eth->src, dev->mac, 6);
	eth->type = htons(ETH_P_IP);

	memset(ip, 0, sizeof(*ip));
	ip->ver_ihl = 0x45;
	ip->total_len = htons(ip_len);
	ip->id = htons(0x4448);
	ip->ttl = 64;
	ip->proto = IP_PROTO_UDP;
	ip->src = htonl(src_ip);
	ip->dst = htonl(dst_ip);
	ip->checksum = htons(checksum(ip, sizeof(*ip)));

	udp->src_port = htons(src_port);
	udp->dst_port = htons(dst_port);
	udp->len = htons(udp_len);
	udp->checksum = 0;
	memcpy(body, payload, payload_len);

	return dev->send(dev, frame, frame_len);
}

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
	return send_ipv4_udp(dev, bcast, 0, 0xffffffffu, DHCP_CLIENT_PORT,
						 DHCP_SERVER_PORT, &pkt,
						 sizeof(pkt) - sizeof(pkt.options) + pos);
}

static void receive_dhcp(netdev_t *dev, const udp_hdr_t *udp, size_t udp_len)
{
	if (udp_len < sizeof(*udp) + sizeof(dhcp_pkt_t) - 312)
		return;
	if (ntohs(udp->src_port) != DHCP_SERVER_PORT ||
		ntohs(udp->dst_port) != DHCP_CLIENT_PORT)
		return;

	const dhcp_pkt_t *pkt = (const dhcp_pkt_t *)((const uint8_t *)udp +
												 sizeof(*udp));
	size_t pkt_len = udp_len - sizeof(*udp);
	if (pkt_len < sizeof(*pkt) - sizeof(pkt->options) ||
		ntohl(pkt->xid) != dhcp_xid || ntohl(pkt->magic) != DHCP_MAGIC ||
		pkt->op != 2 || memcmp(pkt->chaddr, dev->mac, NET_ETH_ALEN) != 0)
		return;

	size_t opt_len = pkt_len - (sizeof(*pkt) - sizeof(pkt->options));
	size_t pos = 0;
	uint8_t type = 0;
	uint32_t server = 0;
	uint32_t mask = 0;
	uint32_t gateway = 0;

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
		pos += len;
	}

	if (type != DHCP_OFFER && type != DHCP_ACK)
		return;

	dhcp_offered_ip = ntohl(pkt->yiaddr);
	dhcp_server_ip = server;
	dhcp_netmask = mask;
	dhcp_gateway = gateway;
	dhcp_seen_type = type;
	dhcp_ready = 1;
}

static int send_arp_request(netdev_t *dev, uint32_t target_ip)
{
	uint8_t frame[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
	eth_hdr_t *eth = (eth_hdr_t *)frame;
	arp_pkt_t *arp = (arp_pkt_t *)(frame + sizeof(*eth));

	memset(eth->dst, 0xff, 6);
	memcpy(eth->src, dev->mac, 6);
	eth->type = htons(ETH_P_ARP);

	arp->htype = htons(1);
	arp->ptype = htons(ETH_P_IP);
	arp->hlen = 6;
	arp->plen = 4;
	arp->oper = htons(1);
	memcpy(arp->sha, dev->mac, 6);
	arp->spa = htonl(dev->ipv4_addr);
	memset(arp->tha, 0, 6);
	arp->tpa = htonl(target_ip);

	return dev->send(dev, frame, sizeof(frame));
}

static int send_icmp_echo(netdev_t *dev, const uint8_t dst_mac[6],
						  uint32_t dst_ip, uint16_t ident, uint16_t seq)
{
	uint8_t frame[sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_echo_t)];
	eth_hdr_t *eth = (eth_hdr_t *)frame;
	ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(*eth));
	icmp_echo_t *icmp = (icmp_echo_t *)((uint8_t *)ip + sizeof(*ip));

	memcpy(eth->dst, dst_mac, 6);
	memcpy(eth->src, dev->mac, 6);
	eth->type = htons(ETH_P_IP);

	memset(ip, 0, sizeof(*ip));
	ip->ver_ihl = 0x45;
	ip->total_len = htons(sizeof(*ip) + sizeof(*icmp));
	ip->id = htons(ident);
	ip->ttl = 64;
	ip->proto = IP_PROTO_ICMP;
	ip->src = htonl(dev->ipv4_addr);
	ip->dst = htonl(dst_ip);
	ip->checksum = htons(checksum(ip, sizeof(*ip)));

	memset(icmp, 0, sizeof(*icmp));
	icmp->type = ICMP_ECHO_REQUEST;
	icmp->ident = htons(ident);
	icmp->seq = htons(seq);
	for (size_t i = 0; i < sizeof(icmp->payload); i++)
		icmp->payload[i] = (uint8_t)i;
	icmp->checksum = htons(checksum(icmp, sizeof(*icmp)));

	return dev->send(dev, frame, sizeof(frame));
}

void net_receive_frame(netdev_t *dev, const void *frame, size_t len)
{
	if (!dev || !frame || len < sizeof(eth_hdr_t))
		return;

	const uint8_t *buf = frame;
	const eth_hdr_t *eth = (const eth_hdr_t *)buf;
	uint16_t type = ntohs(eth->type);

	if (type == ETH_P_ARP && len >= sizeof(*eth) + sizeof(arp_pkt_t)) {
		const arp_pkt_t *arp = (const arp_pkt_t *)(buf + sizeof(*eth));
		if (ntohs(arp->oper) == 2 && ntohl(arp->spa) == arp_ip) {
			memcpy(arp_mac, arp->sha, 6);
			arp_ready = 1;
		}
		return;
	}

	if (type != ETH_P_IP || len < sizeof(*eth) + sizeof(ipv4_hdr_t))
		return;

	const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(buf + sizeof(*eth));
	size_t ihl = (ip->ver_ihl & 0x0f) * 4;
	if (ihl < sizeof(ipv4_hdr_t) || len < sizeof(*eth) + ihl)
		return;
	size_t ip_len = ntohs(ip->total_len);
	if (ip_len < ihl || len < sizeof(*eth) + ip_len)
		return;

	if (ip->proto == IP_PROTO_UDP) {
		if (ip_len < ihl + sizeof(udp_hdr_t))
			return;
		const udp_hdr_t *udp = (const udp_hdr_t *)(buf + sizeof(*eth) + ihl);
		size_t udp_len = ntohs(udp->len);
		if (udp_len < sizeof(*udp) || ihl + udp_len > ip_len)
			return;
		receive_dhcp(dev, udp, udp_len);
		return;
	}

	if (ip->proto != IP_PROTO_ICMP || ntohl(ip->dst) != dev->ipv4_addr)
		return;
	if (ip_len < ihl + sizeof(icmp_echo_t))
		return;
	const icmp_echo_t *icmp =
		(const icmp_echo_t *)(buf + sizeof(*eth) + ihl);
	if (icmp->type == ICMP_ECHO_REPLY && ntohs(icmp->ident) == ping_ident &&
		ntohs(icmp->seq) == ping_seq && ntohl(ip->src) == ping_src) {
		uint64_t hz = pit_get_hz();
		uint64_t elapsed_ticks = pit_get_ticks() - ping_started_tick;
		memset(&ping_result, 0, sizeof(ping_result));
		ping_result.src_ip = ntohl(ip->src);
		ping_result.seq = ntohs(icmp->seq);
		ping_result.bytes = (uint16_t)(ip_len - ihl);
		ping_result.ttl = ip->ttl;
		ping_result.time_ms = hz ? (elapsed_ticks * 1000) / hz : 0;
		ping_ready = 1;
	}
}

static void poll_until(netdev_t *dev, uint64_t until_tick, int *flag)
{
	while (!*flag && pit_get_ticks() < until_tick)
		dev->poll(dev);
}

static uint64_t timeout_ticks(uint64_t timeout_ms)
{
	uint64_t hz = pit_get_hz();
	uint64_t ticks = (timeout_ms * hz + 999) / 1000;
	return ticks ? ticks : 1;
}

int net_dhcp_configure(uint64_t timeout_ms)
{
	netdev_t *dev = netdevs;
	if (!dev)
		return VFS_ERR_NOENT;

	dhcp_xid = 0x4c595244u;
	dhcp_ready = 0;
	dhcp_seen_type = 0;
	dhcp_offered_ip = 0;
	dhcp_server_ip = 0;
	dhcp_netmask = 0;
	dhcp_gateway = 0;

	uint64_t until = pit_get_ticks() + timeout_ticks(timeout_ms);
	int r = send_dhcp(dev, DHCP_DISCOVER, 0, 0);
	if (r != VFS_OK)
		return r;
	poll_until(dev, until, &dhcp_ready);
	if (!dhcp_ready || dhcp_seen_type != DHCP_OFFER || !dhcp_offered_ip)
		return VFS_ERR_NOENT;

	uint32_t offered = dhcp_offered_ip;
	uint32_t server = dhcp_server_ip;
	dhcp_ready = 0;
	dhcp_seen_type = 0;
	until = pit_get_ticks() + timeout_ticks(timeout_ms);
	r = send_dhcp(dev, DHCP_REQUEST, offered, server);
	if (r != VFS_OK)
		return r;
	poll_until(dev, until, &dhcp_ready);
	if (!dhcp_ready || dhcp_seen_type != DHCP_ACK)
		return VFS_ERR_NOENT;

	dev->ipv4_addr = dhcp_offered_ip;
	dev->dhcp_server = dhcp_server_ip;
	dev->ipv4_netmask = dhcp_netmask ? dhcp_netmask : net_ipv4(255, 255, 255, 0);
	dev->ipv4_gateway = dhcp_gateway ? dhcp_gateway : dhcp_server_ip;
	dev->dhcp_configured = 1;

	char ip[24];
	char gw[24];
	char srv[24];
	net_ipv4_format(dev->ipv4_addr, ip, sizeof(ip));
	net_ipv4_format(dev->ipv4_gateway, gw, sizeof(gw));
	net_ipv4_format(dev->dhcp_server, srv, sizeof(srv));
	log_info("net", "%s DHCP lease ip=%s gateway=%s server=%s", dev->name, ip,
			 gw, srv);
	return VFS_OK;
}

int net_ping_echo(uint32_t dst_ip, uint16_t ident, uint16_t seq,
				  uint64_t timeout_ms, net_ping_result_t *result)
{
	netdev_t *dev = netdevs;
	if (!dev)
		return VFS_ERR_NOENT;

	uint32_t next_hop = dst_ip;
	if ((dst_ip & dev->ipv4_netmask) != (dev->ipv4_addr & dev->ipv4_netmask))
		next_hop = dev->ipv4_gateway;

	uint64_t until = pit_get_ticks() + timeout_ticks(timeout_ms);

	arp_ip = next_hop;
	arp_ready = 0;
	send_arp_request(dev, next_hop);
	poll_until(dev, until, &arp_ready);
	if (!arp_ready)
		return VFS_ERR_NOENT;

	ping_ident = ident;
	ping_seq = seq;
	ping_src = dst_ip;
	ping_ready = 0;
	memset(&ping_result, 0, sizeof(ping_result));
	ping_started_tick = pit_get_ticks();
	send_icmp_echo(dev, arp_mac, dst_ip, ident, seq);
	poll_until(dev, until, &ping_ready);
	if (!ping_ready)
		return VFS_ERR_NOENT;
	if (result)
		memcpy(result, &ping_result, sizeof(*result));
	return VFS_OK;
}

int net_ping(uint32_t dst_ip, uint16_t ident, uint16_t seq, uint64_t timeout_ms)
{
	return net_ping_echo(dst_ip, ident, seq, timeout_ms, NULL);
}
