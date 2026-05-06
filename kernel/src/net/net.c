#include "internal.h"
#include <dev/async.h>
#include <debug/log.h>
#include <dev/pit.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <lib/nanoprintf.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <sync/spinlock.h>

static spinlock_t net_lock = SPINLOCK_INIT;
static netdev_t *netdevs;
static netdev_t *default_netdev;

static int loopback_send(netdev_t *dev, const void *frame, size_t len)
{
	if (!dev || !frame || len == 0 || len > NET_ETH_FRAME_MAX)
		return VFS_ERR_INVAL;
	dev->tx_packets++;
	net_receive_frame(dev, frame, len);
	return VFS_OK;
}

static int loopback_poll(netdev_t *dev)
{
	(void)dev;
	return VFS_OK;
}

static int net_poll_dev(netdev_t *dev)
{
	if (!dev || !dev->poll)
		return VFS_ERR_INVAL;
	if (__sync_lock_test_and_set(&dev->poll_busy, 1))
		return VFS_OK;
	int r = dev->poll(dev);
	__sync_lock_release(&dev->poll_busy);
	return r;
}

uint32_t net_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

netdev_t *net_first_dev(void)
{
	return netdevs;
}

netdev_t *net_default_dev(void)
{
	return default_netdev ? default_netdev : netdevs;
}

static size_t netmask_prefix_len(uint32_t mask)
{
	size_t n = 0;
	while (mask & 0x80000000u) {
		n++;
		mask <<= 1;
	}
	return n;
}

netdev_t *net_route(uint32_t dst_ip, uint32_t *next_hop)
{
	netdev_t *best = NULL;
	size_t best_prefix = 0;

	for (netdev_t *dev = netdevs; dev; dev = dev->next) {
		if (!dev->ipv4_addr || !dev->ipv4_netmask)
			continue;
		if ((dst_ip & dev->ipv4_netmask) !=
			(dev->ipv4_addr & dev->ipv4_netmask))
			continue;
		size_t prefix = netmask_prefix_len(dev->ipv4_netmask);
		if (!best || prefix > best_prefix) {
			best = dev;
			best_prefix = prefix;
		}
	}

	if (best) {
		if (next_hop)
			*next_hop = dst_ip;
		return best;
	}

	netdev_t *fallback = net_default_dev();
	if (fallback && next_hop)
		*next_hop = fallback->ipv4_gateway ? fallback->ipv4_gateway : dst_ip;
	return fallback;
}

uint32_t net_default_ipv4(void)
{
	netdev_t *dev = net_default_dev();
	return dev ? dev->ipv4_addr : 0;
}

uint32_t net_default_gateway(void)
{
	netdev_t *dev = net_default_dev();
	return dev ? dev->ipv4_gateway : 0;
}

uint16_t net_checksum(const void *data, size_t len)
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

uint16_t net_checksum2(const void *a, size_t a_len, const void *b,
					   size_t b_len)
{
	const uint8_t *pa = a;
	const uint8_t *pb = b;
	uint32_t sum = 0;

	while (a_len > 1) {
		sum += ((uint16_t)pa[0] << 8) | pa[1];
		pa += 2;
		a_len -= 2;
	}
	if (a_len)
		sum += (uint16_t)pa[0] << 8;

	while (b_len > 1) {
		sum += ((uint16_t)pb[0] << 8) | pb[1];
		pb += 2;
		b_len -= 2;
	}
	if (b_len)
		sum += (uint16_t)pb[0] << 8;

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
							 "%s mac=%02x:%02x:%02x:%02x:%02x:%02x ip=%s gateway=%s mtu=%u dhcp=%s link=%s%s%s\n",
							 dev->name, dev->mac[0], dev->mac[1],
							 dev->mac[2], dev->mac[3], dev->mac[4],
							 dev->mac[5], ip, gw, dev->mtu,
							 dev->dhcp_configured ? "yes" : "no",
							 dev->link_up ? "up" : "down",
							 dev->loopback ? " loopback" : "",
							 dev == net_default_dev() ? " default" : "");
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

static int routes_read(void *ctx, uint64_t off, void *buf, size_t len,
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
		if (dev->ipv4_addr && dev->ipv4_netmask) {
			char net[24];
			char mask[24];
			net_ipv4_format(dev->ipv4_addr & dev->ipv4_netmask, net,
							sizeof(net));
			net_ipv4_format(dev->ipv4_netmask, mask, sizeof(mask));
			int n = npf_snprintf(tmp + pos, sizeof(tmp) - pos,
								 "%s/%s dev %s\n", net, mask, dev->name);
			if (n < 0)
				break;
			if ((size_t)n >= sizeof(tmp) - pos) {
				pos = sizeof(tmp) - 1;
				break;
			}
			pos += (size_t)n;
		}
		if (dev == net_default_dev() && dev->ipv4_gateway) {
			char gw[24];
			net_ipv4_format(dev->ipv4_gateway, gw, sizeof(gw));
			int n = npf_snprintf(tmp + pos, sizeof(tmp) - pos,
								 "0.0.0.0/0.0.0.0 via %s dev %s default\n",
								 gw, dev->name);
			if (n < 0)
				break;
			if ((size_t)n >= sizeof(tmp) - pos) {
				pos = sizeof(tmp) - 1;
				break;
			}
			pos += (size_t)n;
		}
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

static void netdev_enqueue_rx(netdev_t *dev, const void *frame, size_t len)
{
	if (!dev || !frame || len == 0 || len > NET_ETH_FRAME_MAX)
		return;

	spinlock_acquire(&net_lock);
	if (dev->rx_count == NETDEV_RX_QUEUE_LEN) {
		dev->rx_dropped++;
		spinlock_release(&net_lock);
		return;
	}

	size_t idx = dev->rx_tail;
	memcpy(dev->rx_queue + idx * NET_ETH_FRAME_MAX, frame, len);
	dev->rx_len[idx] = (uint16_t)len;
	dev->rx_tail = (idx + 1) % NETDEV_RX_QUEUE_LEN;
	dev->rx_count++;
	dev->rx_packets++;
	spinlock_release(&net_lock);
}

static int netdev_chr_read(void *ctx, uint64_t off, void *buf, size_t len,
						   size_t *done)
{
	(void)off;
	netdev_t *dev = ctx;
	if (done)
		*done = 0;
	if (!dev || !buf)
		return VFS_ERR_INVAL;

	net_poll_dev(dev);

	spinlock_acquire(&net_lock);
	if (!dev->rx_count) {
		spinlock_release(&net_lock);
		return VFS_OK;
	}

	size_t idx = dev->rx_head;
	size_t frame_len = dev->rx_len[idx];
	if (len < frame_len) {
		spinlock_release(&net_lock);
		return VFS_ERR_INVAL;
	}

	memcpy(buf, dev->rx_queue + idx * NET_ETH_FRAME_MAX, frame_len);
	dev->rx_len[idx] = 0;
	dev->rx_head = (idx + 1) % NETDEV_RX_QUEUE_LEN;
	dev->rx_count--;
	spinlock_release(&net_lock);

	if (done)
		*done = frame_len;
	return VFS_OK;
}

static int netdev_chr_write(void *ctx, uint64_t off, const void *buf,
							size_t len, size_t *done)
{
	(void)off;
	netdev_t *dev = ctx;
	if (done)
		*done = 0;
	if (!dev || !buf || len == 0 || len > NET_ETH_FRAME_MAX)
		return VFS_ERR_INVAL;

	int r = dev->send(dev, buf, len);
	if (r != VFS_OK)
		return r;
	dev->tx_packets++;
	if (done)
		*done = len;
	return VFS_OK;
}

static int netdev_info_read(void *ctx, uint64_t off, void *buf, size_t len,
							size_t *done)
{
	netdev_t *dev = ctx;
	if (done)
		*done = 0;
	if (!dev || !buf)
		return VFS_ERR_INVAL;

	char ip[24];
	char gw[24];
	char mask[24];
	char server[24];
	char dns[24];
	char route[24];
	net_ipv4_format(dev->ipv4_addr, ip, sizeof(ip));
	net_ipv4_format(dev->ipv4_gateway, gw, sizeof(gw));
	net_ipv4_format(dev->ipv4_netmask, mask, sizeof(mask));
	net_ipv4_format(dev->dhcp_server, server, sizeof(server));
	net_ipv4_format(dev->dns_server, dns, sizeof(dns));
	uint32_t route_next = 0;
	netdev_t *route_dev = net_route(net_ipv4(8, 8, 8, 8), &route_next);
	net_ipv4_format(route_next, route, sizeof(route));

	char tmp[768];
	int n = npf_snprintf(
		tmp, sizeof(tmp),
		"name=%s\nmac=%02x:%02x:%02x:%02x:%02x:%02x\nlink=%s\nloopback=%s\ndefault=%s\nmtu=%u\nip=%s\nnetmask=%s\ngateway=%s\ndhcp=%s\ndhcp_server=%s\ndns_server=%s\nroute_8_8_8_8=%s via %s\narp_cache_entries=%zu\nrx_packets=%llu\ntx_packets=%llu\nrx_dropped=%llu\nrx_queued=%zu\n",
		dev->name, dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3],
		dev->mac[4], dev->mac[5], dev->link_up ? "up" : "down",
		dev->loopback ? "yes" : "no",
		dev == net_default_dev() ? "yes" : "no", dev->mtu, ip, mask, gw,
		dev->dhcp_configured ? "yes" : "no", server, dns,
		route_dev == dev ? "selected" : "not-selected", route,
		net_arp_cache_count(dev),
		(unsigned long long)dev->rx_packets,
		(unsigned long long)dev->tx_packets,
		(unsigned long long)dev->rx_dropped, dev->rx_count);
	if (n < 0)
		return VFS_ERR_INVAL;

	size_t total = (size_t)n;
	if (total >= sizeof(tmp))
		total = sizeof(tmp) - 1;
	if (off >= total || len == 0)
		return VFS_OK;
	size_t copy = total - (size_t)off;
	if (copy > len)
		copy = len;
	memcpy(buf, tmp + off, copy);
	if (done)
		*done = copy;
	return VFS_OK;
}

static int netdev_publish(netdev_t *dev)
{
	char path[64];
	npf_snprintf(path, sizeof(path), "/dev/net/%s", dev->name);
	int r = devfs_register_chr(path, 0660, netdev_chr_read, netdev_chr_write,
							   dev);
	if (r != VFS_OK)
		return r;

	npf_snprintf(path, sizeof(path), "/dev/net/%s.info", dev->name);
	r = devfs_register_chr(path, 0444, netdev_info_read, NULL, dev);
	if (r != VFS_OK) {
		char raw_path[64];
		npf_snprintf(raw_path, sizeof(raw_path), "/dev/net/%s", dev->name);
		devfs_unregister(raw_path);
		return r;
	}
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
	r = devfs_register_chr("/dev/net/routes", 0444, routes_read, NULL, NULL);
	if (r != VFS_OK && r != VFS_ERR_EXIST)
		return r;

	netdev_t lo;
	memset(&lo, 0, sizeof(lo));
	npf_snprintf(lo.name, sizeof(lo.name), "lo");
	lo.link_up = 1;
	lo.loopback = 1;
	lo.ipv4_addr = net_ipv4(127, 0, 0, 1);
	lo.ipv4_netmask = net_ipv4(255, 0, 0, 0);
	lo.mtu = 65535;
	lo.send = loopback_send;
	lo.poll = loopback_poll;
	r = netdev_register(&lo);
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
	dev->rx_queue = NULL;
	memset(dev->rx_len, 0, sizeof(dev->rx_len));
	dev->rx_head = 0;
	dev->rx_tail = 0;
	dev->rx_count = 0;
	dev->rx_dropped = 0;
	dev->rx_packets = 0;
	dev->tx_packets = 0;
	dev->rx_queue = kzalloc(NETDEV_RX_QUEUE_LEN * NET_ETH_FRAME_MAX);
	if (!dev->rx_queue) {
		kfree(dev);
		return VFS_ERR_NOMEM;
	}
	if (!dev->mtu)
		dev->mtu = NET_MTU;

	spinlock_acquire(&net_lock);
	for (netdev_t *cur = netdevs; cur; cur = cur->next) {
		if (strcmp(cur->name, dev->name) == 0) {
			spinlock_release(&net_lock);
			kfree(dev->rx_queue);
			kfree(dev);
			return VFS_ERR_EXIST;
		}
	}
	spinlock_release(&net_lock);

	int pr = netdev_publish(dev);
	if (pr != VFS_OK) {
		kfree(dev->rx_queue);
		kfree(dev);
		return pr;
	}

	spinlock_acquire(&net_lock);
	if (!netdevs) {
		netdevs = dev;
		if (!dev->loopback)
			default_netdev = dev;
	} else {
		netdev_t *tail = netdevs;
		while (tail->next)
			tail = tail->next;
		tail->next = dev;
		if (!default_netdev && !dev->loopback)
			default_netdev = dev;
	}
	spinlock_release(&net_lock);

	log_info("net", "%s registered mac=%02x:%02x:%02x:%02x:%02x:%02x",
			 dev->name, dev->mac[0], dev->mac[1], dev->mac[2], dev->mac[3],
			 dev->mac[4], dev->mac[5]);

	if (dev->loopback)
		return VFS_OK;

	int r = net_dhcp_configure_dev(dev, 2000);
	if (r != VFS_OK) {
		if (!dev->ipv4_addr)
			dev->ipv4_addr = net_ipv4(10, 0, 2, 15);
		if (!dev->ipv4_gateway)
			dev->ipv4_gateway = net_ipv4(10, 0, 2, 2);
		if (!dev->ipv4_netmask)
			dev->ipv4_netmask = net_ipv4(255, 255, 255, 0);
		if (!dev->dns_server)
			dev->dns_server = net_ipv4(10, 0, 2, 3);
		log_warn("net", "%s DHCP failed status=%d, using %u.%u.%u.%u",
				 dev->name, r, (dev->ipv4_addr >> 24) & 0xff,
				 (dev->ipv4_addr >> 16) & 0xff,
				 (dev->ipv4_addr >> 8) & 0xff, dev->ipv4_addr & 0xff);
	}
	return VFS_OK;
}

void net_receive_frame(netdev_t *dev, const void *frame, size_t len)
{
	if (!dev || !frame || len < sizeof(eth_hdr_t))
		return;

	netdev_enqueue_rx(dev, frame, len);

	const uint8_t *buf = frame;
	const eth_hdr_t *eth = (const eth_hdr_t *)buf;
	uint16_t type = ntohs(eth->type);

	if (type == ETH_P_ARP && len >= sizeof(*eth) + sizeof(arp_pkt_t)) {
		net_arp_receive(dev, (const arp_pkt_t *)(buf + sizeof(*eth)));
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
		net_dhcp_receive(dev, udp, udp_len);
		net_dns_receive(dev, udp, udp_len);
		return;
	}

	if (ip->proto == IP_PROTO_TCP) {
		net_tcp_receive(dev, eth->src, ip, ihl, ip_len);
		return;
	}

	if (ip->proto == IP_PROTO_ICMP)
		net_icmp_receive(dev, ip, ihl, ip_len);
}

void net_poll_until(netdev_t *dev, uint64_t until_tick, int *flag)
{
	if (!flag)
		return;

	while (!*flag && pit_get_ticks() < until_tick) {
		async_io_drain(16);
		if (dev)
			net_poll_dev(dev);
		else
			net_poll_all();
		__asm__ volatile("pause" ::: "memory");
	}
}

void net_poll_all(void)
{
	for (netdev_t *dev = netdevs; dev; dev = dev->next)
		net_poll_dev(dev);
}

size_t netdev_count(void)
{
	size_t count = 0;
	for (netdev_t *dev = netdevs; dev; dev = dev->next)
		count++;
	return count;
}

uint64_t net_timeout_ticks(uint64_t timeout_ms)
{
	uint64_t hz = pit_get_hz();
	uint64_t ticks = (timeout_ms * hz + 999) / 1000;
	return ticks ? ticks : 1;
}
