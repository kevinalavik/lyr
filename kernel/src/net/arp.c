#include "internal.h"
#include <dev/pit.h>
#include <fs/vfs.h>
#include <lib/string.h>

static uint8_t arp_mac[NET_ETH_ALEN];
static uint32_t arp_ip;
static int arp_ready;

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
		return VFS_ERR_INVAL;

	arp_ip = target_ip;
	arp_ready = 0;
	int r = send_arp_request(dev, target_ip);
	if (r != VFS_OK)
		return r;

	uint64_t until = pit_get_ticks() + net_timeout_ticks(timeout_ms);
	net_poll_until(dev, until, &arp_ready);
	if (!arp_ready)
		return VFS_ERR_NOENT;

	memcpy(out_mac, arp_mac, NET_ETH_ALEN);
	return VFS_OK;
}

void net_arp_receive(const arp_pkt_t *arp)
{
	if (ntohs(arp->oper) == 2 && ntohl(arp->spa) == arp_ip) {
		memcpy(arp_mac, arp->sha, NET_ETH_ALEN);
		arp_ready = 1;
	}
}
