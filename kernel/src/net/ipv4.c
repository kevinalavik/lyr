#include "internal.h"
#include <fs/vfs.h>
#include <lib/string.h>

#define TCP_WINDOW 4096

int net_send_ipv4_udp(netdev_t *dev, const uint8_t dst_mac[6],
					  uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
					  uint16_t dst_port, const void *payload,
					  size_t payload_len)
{
	if (!dev || !payload || payload_len > 1400)
		return VFS_ERR_INVAL;

	uint8_t frame[NET_ETH_FRAME_MAX];
	size_t udp_len = sizeof(udp_hdr_t) + payload_len;
	size_t ip_len = sizeof(ipv4_hdr_t) + udp_len;
	size_t frame_len = sizeof(eth_hdr_t) + ip_len;
	if (frame_len > sizeof(frame))
		return VFS_ERR_INVAL;

	eth_hdr_t *eth = (eth_hdr_t *)frame;
	ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(*eth));
	udp_hdr_t *udp = (udp_hdr_t *)((uint8_t *)ip + sizeof(*ip));
	uint8_t *body = (uint8_t *)udp + sizeof(*udp);

	memcpy(eth->dst, dst_mac, NET_ETH_ALEN);
	memcpy(eth->src, dev->mac, NET_ETH_ALEN);
	eth->type = htons(ETH_P_IP);

	memset(ip, 0, sizeof(*ip));
	ip->ver_ihl = 0x45;
	ip->total_len = htons(ip_len);
	ip->id = htons(0x4448);
	ip->ttl = 64;
	ip->proto = IP_PROTO_UDP;
	ip->src = htonl(src_ip);
	ip->dst = htonl(dst_ip);
	ip->checksum = htons(net_checksum(ip, sizeof(*ip)));

	udp->src_port = htons(src_port);
	udp->dst_port = htons(dst_port);
	udp->len = htons(udp_len);
	udp->checksum = 0;
	memcpy(body, payload, payload_len);

	return dev->send(dev, frame, frame_len);
}

int net_send_ipv4_tcp(netdev_t *dev, const uint8_t dst_mac[6],
					  uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
					  uint32_t seq, uint32_t ack, uint8_t flags,
					  const void *payload, size_t payload_len)
{
	if (!dev || !dst_mac || payload_len > 1400)
		return VFS_ERR_INVAL;

	uint8_t frame[NET_ETH_FRAME_MAX];
	size_t tcp_len = sizeof(tcp_hdr_t) + payload_len;
	size_t ip_len = sizeof(ipv4_hdr_t) + tcp_len;
	size_t frame_len = sizeof(eth_hdr_t) + ip_len;
	if (frame_len > sizeof(frame))
		return VFS_ERR_INVAL;

	eth_hdr_t *eth = (eth_hdr_t *)frame;
	ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(*eth));
	tcp_hdr_t *tcp = (tcp_hdr_t *)((uint8_t *)ip + sizeof(*ip));
	uint8_t *body = (uint8_t *)tcp + sizeof(*tcp);

	memcpy(eth->dst, dst_mac, NET_ETH_ALEN);
	memcpy(eth->src, dev->mac, NET_ETH_ALEN);
	eth->type = htons(ETH_P_IP);

	memset(ip, 0, sizeof(*ip));
	ip->ver_ihl = 0x45;
	ip->total_len = htons(ip_len);
	ip->id = htons(0x5443);
	ip->ttl = 64;
	ip->proto = IP_PROTO_TCP;
	ip->src = htonl(dev->ipv4_addr);
	ip->dst = htonl(dst_ip);
	ip->checksum = htons(net_checksum(ip, sizeof(*ip)));

	memset(tcp, 0, sizeof(*tcp));
	tcp->src_port = htons(src_port);
	tcp->dst_port = htons(dst_port);
	tcp->seq = htonl(seq);
	tcp->ack = htonl(ack);
	tcp->data_off = (uint8_t)(sizeof(*tcp) / 4) << 4;
	tcp->flags = flags;
	tcp->window = htons(TCP_WINDOW);
	if (payload_len)
		memcpy(body, payload, payload_len);

	ipv4_pseudo_t pseudo;
	pseudo.src = htonl(dev->ipv4_addr);
	pseudo.dst = htonl(dst_ip);
	pseudo.zero = 0;
	pseudo.proto = IP_PROTO_TCP;
	pseudo.len = htons(tcp_len);
	tcp->checksum = htons(net_checksum2(&pseudo, sizeof(pseudo), tcp, tcp_len));

	return dev->send(dev, frame, frame_len);
}
