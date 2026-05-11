#include "../stack.h"
#include <errno.h>
#include <lib/string.h>

static int udp_ipv4_receive(const net_ipv4_rx_info_t *rx, void *ctx);

static const net_ipv4_protocol_ops_t udp_ipv4_protocol = {
	.protocol = IP_PROTO_UDP,
	.name = "udp",
	.receive = udp_ipv4_receive,
	.ctx = NULL,
};

int net_send_ipv4_udp(netdev_t *dev, const uint8_t dst_mac[6],
					  uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
					  uint16_t dst_port, const void *payload,
					  size_t payload_len)
{
	if (!dev || !payload || payload_len > 1400)
		return -EINVAL;

	uint8_t frame[NET_ETH_FRAME_MAX];
	size_t udp_len = sizeof(udp_hdr_t) + payload_len;
	size_t ip_len = sizeof(ipv4_hdr_t) + udp_len;
	size_t frame_len = sizeof(eth_hdr_t) + ip_len;
	if (frame_len > sizeof(frame))
		return -EINVAL;

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

	if (dst_ip == dev->ipv4_addr) {
		dev->tx_packets++;
		net_receive_frame(dev, frame, frame_len);
		return 0;
	}

	return dev->send(dev, frame, frame_len);
}

uint32_t net_udp_resolve_remote_ip(netdev_t *dev, uint32_t requested_ip,
								   uint16_t requested_port)
{
	if (requested_port == 53 && dev && dev->dns_server &&
		dev->dns_server != requested_ip)
		return dev->dns_server;

	return requested_ip;
}

int net_udp_peer_matches(netdev_t *dev, uint32_t requested_ip,
						 uint16_t requested_port, uint32_t src_ip,
						 uint16_t src_port)
{
	if (requested_port == 53 && src_port == 53) {
		if (src_ip == requested_ip)
			return 1;
		if (dev && dev->dns_server && src_ip == dev->dns_server)
			return 1;
	}

	return requested_ip == src_ip && requested_port == src_port;
}

static int udp_ipv4_receive(const net_ipv4_rx_info_t *rx, void *ctx)
{
	(void)ctx;

	if (!rx || !rx->ip || rx->ip_len < rx->ihl + sizeof(udp_hdr_t))
		return -EINVAL;

	const udp_hdr_t *udp =
		(const udp_hdr_t *)((const uint8_t *)rx->ip + rx->ihl);
	size_t udp_len = ntohs(udp->len);
	if (udp_len < sizeof(*udp) || rx->ihl + udp_len > rx->ip_len)
		return -EINVAL;

	net_udp_dgram_t dgram = {
		.ipv4 = rx,
		.udp = udp,
		.payload = (const uint8_t *)udp + sizeof(*udp),
		.udp_len = udp_len,
		.payload_len = udp_len - sizeof(*udp),
		.src_ip = ntohl(rx->ip->src),
		.dst_ip = ntohl(rx->ip->dst),
		.src_port = ntohs(udp->src_port),
		.dst_port = ntohs(udp->dst_port),
	};

	return net_udp_deliver(&dgram);
}

int net_udp_init(void)
{
	return net_ipv4_register_protocol(&udp_ipv4_protocol);
}
