#include "internal.h"
#include <dev/pit.h>
#include <fs/vfs.h>
#include <lib/string.h>

#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO_REQUEST 8

static int ping_ready;
static uint16_t ping_ident;
static uint16_t ping_seq;
static uint32_t ping_src;
static netdev_t *ping_dev;
static uint64_t ping_started_tick;
static net_ping_result_t ping_result;

static int send_icmp_echo_packet(netdev_t *dev, const uint8_t dst_mac[6],
								 uint32_t src_ip, uint32_t dst_ip,
								 uint8_t type, uint16_t ident, uint16_t seq,
								 const uint8_t payload[32])
{
	uint8_t frame[sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(icmp_echo_t)];
	eth_hdr_t *eth = (eth_hdr_t *)frame;
	ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(*eth));
	icmp_echo_t *icmp = (icmp_echo_t *)((uint8_t *)ip + sizeof(*ip));

	memcpy(eth->dst, dst_mac, NET_ETH_ALEN);
	memcpy(eth->src, dev->mac, NET_ETH_ALEN);
	eth->type = htons(ETH_P_IP);

	memset(ip, 0, sizeof(*ip));
	ip->ver_ihl = 0x45;
	ip->total_len = htons(sizeof(*ip) + sizeof(*icmp));
	ip->id = htons(ident);
	ip->ttl = 64;
	ip->proto = IP_PROTO_ICMP;
	ip->src = htonl(src_ip);
	ip->dst = htonl(dst_ip);
	ip->checksum = htons(net_checksum(ip, sizeof(*ip)));

	memset(icmp, 0, sizeof(*icmp));
	icmp->type = type;
	icmp->ident = htons(ident);
	icmp->seq = htons(seq);
	if (payload) {
		memcpy(icmp->payload, payload, sizeof(icmp->payload));
	} else {
		for (size_t i = 0; i < sizeof(icmp->payload); i++)
			icmp->payload[i] = (uint8_t)i;
	}
	icmp->checksum = htons(net_checksum(icmp, sizeof(*icmp)));

	return dev->send(dev, frame, sizeof(frame));
}

static int send_icmp_echo(netdev_t *dev, const uint8_t dst_mac[6],
						  uint32_t dst_ip, uint16_t ident, uint16_t seq)
{
	return send_icmp_echo_packet(dev, dst_mac, dev->ipv4_addr, dst_ip,
								 ICMP_ECHO_REQUEST, ident, seq, NULL);
}

void net_icmp_receive(netdev_t *dev, const ipv4_hdr_t *ip, size_t ihl,
					  size_t ip_len)
{
	if (!dev || ntohl(ip->dst) != dev->ipv4_addr)
		return;
	if (ip_len < ihl + sizeof(icmp_echo_t))
		return;

	const icmp_echo_t *icmp = (const icmp_echo_t *)((const uint8_t *)ip + ihl);
	net_socket_raw_icmp_receive(dev, ip, ihl, ip_len);

	if (icmp->type == ICMP_ECHO_REQUEST) {
		send_icmp_echo_packet(dev, dev->mac, ntohl(ip->dst), ntohl(ip->src),
							  ICMP_ECHO_REPLY, ntohs(icmp->ident),
							  ntohs(icmp->seq), icmp->payload);
		return;
	}

	if (dev != ping_dev)
		return;
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

int net_ping_echo(uint32_t dst_ip, uint16_t ident, uint16_t seq,
				  uint64_t timeout_ms, net_ping_result_t *result)
{
	netdev_t *dev = net_route(dst_ip, NULL);
	if (!dev)
		return -ENOENT;
	return net_ping_echo_dev(dev, dst_ip, ident, seq, timeout_ms, result);
}

int net_ping_echo_dev(netdev_t *dev, uint32_t dst_ip, uint16_t ident,
					  uint16_t seq, uint64_t timeout_ms,
					  net_ping_result_t *result)
{
	if (!dev)
		return -ENOENT;

	uint32_t next_hop = dst_ip;
	if ((dst_ip & dev->ipv4_netmask) != (dev->ipv4_addr & dev->ipv4_netmask))
		next_hop = dev->ipv4_gateway;

	uint8_t mac[NET_ETH_ALEN];
	int r = net_arp_resolve(dev, next_hop, timeout_ms, mac);
	if (r != 0)
		return r;

	ping_ident = ident;
	ping_seq = seq;
	ping_src = dst_ip;
	ping_dev = dev;
	ping_ready = 0;
	memset(&ping_result, 0, sizeof(ping_result));
	ping_started_tick = pit_get_ticks();
	send_icmp_echo(dev, mac, dst_ip, ident, seq);
	uint64_t until = pit_get_ticks() + net_timeout_ticks(timeout_ms);
	net_poll_until(dev, until, &ping_ready);
	if (!ping_ready)
		return -ENOENT;
	if (result)
		memcpy(result, &ping_result, sizeof(*result));
	return 0;
}

int net_ping(uint32_t dst_ip, uint16_t ident, uint16_t seq, uint64_t timeout_ms)
{
	return net_ping_echo(dst_ip, ident, seq, timeout_ms, NULL);
}
