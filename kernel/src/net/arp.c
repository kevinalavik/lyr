#include "internal.h"
#include <dev/pit.h>
#include <fs/vfs.h>
#include <lib/string.h>

#define ARP_CACHE_LEN 32
#define ARP_CACHE_TTL_MS 60000

typedef struct {
	netdev_t *dev;
	uint32_t ip;
	uint8_t mac[NET_ETH_ALEN];
	uint64_t expires_tick;
} arp_cache_entry_t;

static arp_cache_entry_t arp_cache[ARP_CACHE_LEN];
static uint8_t arp_mac[NET_ETH_ALEN];
static netdev_t *arp_dev;
static uint32_t arp_ip;
static int arp_ready;

static int arp_cache_lookup(netdev_t *dev, uint32_t ip,
							uint8_t out_mac[NET_ETH_ALEN])
{
	uint64_t now = pit_get_ticks();
	for (size_t i = 0; i < ARP_CACHE_LEN; i++) {
		arp_cache_entry_t *ent = &arp_cache[i];
		if (ent->dev != dev || ent->ip != ip || now >= ent->expires_tick)
			continue;
		memcpy(out_mac, ent->mac, NET_ETH_ALEN);
		return 1;
	}
	return 0;
}

static void arp_cache_store(netdev_t *dev, uint32_t ip,
							const uint8_t mac[NET_ETH_ALEN])
{
	uint64_t now = pit_get_ticks();
	uint64_t ttl = net_timeout_ticks(ARP_CACHE_TTL_MS);
	size_t victim = 0;

	for (size_t i = 0; i < ARP_CACHE_LEN; i++) {
		arp_cache_entry_t *ent = &arp_cache[i];
		if (ent->dev == dev && ent->ip == ip) {
			victim = i;
			break;
		}
		if (!ent->dev || now >= ent->expires_tick) {
			victim = i;
			break;
		}
	}

	arp_cache[victim].dev = dev;
	arp_cache[victim].ip = ip;
	memcpy(arp_cache[victim].mac, mac, NET_ETH_ALEN);
	arp_cache[victim].expires_tick = now + ttl;
}

static int send_arp_request(netdev_t *dev, uint32_t target_ip)
{
	uint8_t frame[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
	eth_hdr_t *eth = (eth_hdr_t *)frame;
	arp_pkt_t *arp = (arp_pkt_t *)(frame + sizeof(*eth));

	memset(eth->dst, 0xff, NET_ETH_ALEN);
	memcpy(eth->src, dev->mac, NET_ETH_ALEN);
	eth->type = htons(ETH_P_ARP);

	arp->htype = htons(1);
	arp->ptype = htons(ETH_P_IP);
	arp->hlen = NET_ETH_ALEN;
	arp->plen = 4;
	arp->oper = htons(1);
	memcpy(arp->sha, dev->mac, NET_ETH_ALEN);
	arp->spa = htonl(dev->ipv4_addr);
	memset(arp->tha, 0, NET_ETH_ALEN);
	arp->tpa = htonl(target_ip);

	return dev->send(dev, frame, sizeof(frame));
}

int net_arp_resolve(netdev_t *dev, uint32_t target_ip, uint64_t timeout_ms,
					uint8_t out_mac[NET_ETH_ALEN])
{
	if (!dev || !out_mac)
		return -EINVAL;

	/*
	 * Traffic to the address assigned to this interface is local traffic.
	 * Do not ARP for our own IPv4 address: no peer will answer, and callers
	 * such as the init interface probe will stall until the ARP timeout.
	 * Return the device MAC; the IPv4 send path will short-circuit such frames
	 * back into net_receive_frame().
	 */
	if (dev->loopback || target_ip == dev->ipv4_addr) {
		memcpy(out_mac, dev->mac, NET_ETH_ALEN);
		return 0;
	}

	if (arp_cache_lookup(dev, target_ip, out_mac))
		return 0;

	arp_dev = dev;
	arp_ip = target_ip;
	arp_ready = 0;
	int r = send_arp_request(dev, target_ip);
	if (r != 0)
		return r;

	uint64_t until = pit_get_ticks() + net_timeout_ticks(timeout_ms);
	net_poll_until(dev, until, &arp_ready);
	if (!arp_ready)
		return -ENOENT;

	memcpy(out_mac, arp_mac, NET_ETH_ALEN);
	arp_cache_store(dev, target_ip, arp_mac);
	return 0;
}

size_t net_arp_cache_count(netdev_t *dev)
{
	uint64_t now = pit_get_ticks();
	size_t count = 0;
	for (size_t i = 0; i < ARP_CACHE_LEN; i++) {
		arp_cache_entry_t *ent = &arp_cache[i];
		if (ent->dev == dev && now < ent->expires_tick)
			count++;
	}
	return count;
}

void net_arp_receive(netdev_t *dev, const arp_pkt_t *arp)
{
	if (ntohs(arp->oper) != 2)
		return;

	uint32_t sender_ip = ntohl(arp->spa);
	arp_cache_store(dev, sender_ip, arp->sha);

	if (dev == arp_dev && sender_ip == arp_ip) {
		memcpy(arp_mac, arp->sha, NET_ETH_ALEN);
		arp_ready = 1;
	}
}
