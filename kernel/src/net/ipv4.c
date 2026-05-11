#include "internal.h"
#include <fs/vfs.h>
#include <lib/string.h>

#define TCP_WINDOW 65535

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

	/*
	 * If the destination is this interface's own address, deliver the frame
	 * through the local RX path instead of handing it to the NIC. Real hardware
	 * usually will not reflect a unicast frame addressed to our own MAC back to
	 * us, but local sockets must still be able to connect to the interface IP.
	 */
	if (dst_ip == dev->ipv4_addr) {
		dev->tx_packets++;
		net_receive_frame(dev, frame, frame_len);
		return 0;
	}

	return dev->send(dev, frame, frame_len);
}

int net_send_ipv4_icmp(netdev_t *dev, const uint8_t dst_mac[6],
					   uint32_t src_ip, uint32_t dst_ip,
					   const void *payload, size_t payload_len)
{
	if (!dev || !payload || payload_len > 1400)
		return -EINVAL;

	uint8_t frame[NET_ETH_FRAME_MAX];
	size_t ip_len = sizeof(ipv4_hdr_t) + payload_len;
	size_t frame_len = sizeof(eth_hdr_t) + ip_len;
	if (frame_len > sizeof(frame))
		return -EINVAL;

	eth_hdr_t *eth = (eth_hdr_t *)frame;
	ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(*eth));
	uint8_t *body = (uint8_t *)ip + sizeof(*ip);

	memcpy(eth->dst, dst_mac, NET_ETH_ALEN);
	memcpy(eth->src, dev->mac, NET_ETH_ALEN);
	eth->type = htons(ETH_P_IP);

	memset(ip, 0, sizeof(*ip));
	ip->ver_ihl = 0x45;
	ip->total_len = htons(ip_len);
	ip->id = htons(0x4943);
	ip->ttl = 64;
	ip->proto = IP_PROTO_ICMP;
	ip->src = htonl(src_ip);
	ip->dst = htonl(dst_ip);
	ip->checksum = htons(net_checksum(ip, sizeof(*ip)));

	memcpy(body, payload, payload_len);
	if (payload_len >= 4) {
		/*
		 * Raw ICMP sockets hand us the userspace payload verbatim. Normalize the
		 * checksum here so externally routed echo requests are valid even if the
		 * caller used host-endian words while building the packet.
		 */
		body[2] = 0;
		body[3] = 0;
		uint16_t checksum = htons(net_checksum(body, payload_len));
		memcpy(body + 2, &checksum, sizeof(checksum));
	}

	if (dst_ip == dev->ipv4_addr) {
		dev->tx_packets++;
		net_receive_frame(dev, frame, frame_len);
		return 0;
	}

	return dev->send(dev, frame, frame_len);
}

static int net_send_ipv4_tcp_with_window_opts_impl(
	netdev_t *dev, const uint8_t dst_mac[6], uint32_t dst_ip,
	uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack,
	uint8_t flags, uint16_t window, int include_mss, int include_wscale,
	uint8_t wscale, int include_sack_permitted, const void *payload,
	size_t payload_len)
{
	if (!dev || !dst_mac || payload_len > 1400)
		return -EINVAL;

	uint8_t frame[NET_ETH_FRAME_MAX];
	size_t tcp_opt_len = 0;

	if (flags & TCP_SYN) {
		if (include_mss)
			tcp_opt_len += 4;
		if (include_sack_permitted)
			tcp_opt_len += 2;
		if (include_wscale)
			tcp_opt_len += 4; /* NOP, kind, len, shift */
		if (tcp_opt_len & 3)
			tcp_opt_len = (tcp_opt_len + 3) & ~(size_t)3;
	}

	size_t tcp_hlen = sizeof(tcp_hdr_t) + tcp_opt_len;
	size_t tcp_len = tcp_hlen + payload_len;
	size_t ip_len = sizeof(ipv4_hdr_t) + tcp_len;
	size_t frame_len = sizeof(eth_hdr_t) + ip_len;
	if (frame_len > sizeof(frame))
		return -EINVAL;

	eth_hdr_t *eth = (eth_hdr_t *)frame;
	ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(*eth));
	tcp_hdr_t *tcp = (tcp_hdr_t *)((uint8_t *)ip + sizeof(*ip));
	uint8_t *tcp_opts = (uint8_t *)tcp + sizeof(*tcp);
	uint8_t *body = (uint8_t *)tcp + tcp_hlen;

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
	tcp->data_off = (uint8_t)(tcp_hlen / 4) << 4;
	tcp->flags = flags;
	tcp->window = htons(window);
	if (tcp_opt_len) {
		size_t opt = 0;

		if (include_mss) {
			tcp_opts[opt++] = 2;  /* MSS */
			tcp_opts[opt++] = 4;
			tcp_opts[opt++] = 0x05;
			tcp_opts[opt++] = 0xb4; /* 1460 */
		}

		if (include_sack_permitted) {
			tcp_opts[opt++] = 4;  /* SACK permitted */
			tcp_opts[opt++] = 2;
		}

		if (include_wscale) {
			if (wscale > 14)
				wscale = 14;
			tcp_opts[opt++] = 1;  /* NOP for alignment */
			tcp_opts[opt++] = 3;  /* window scale */
			tcp_opts[opt++] = 3;
			tcp_opts[opt++] = wscale;
		}

		while (opt < tcp_opt_len)
			tcp_opts[opt++] = 0; /* EOL/padding */
	}
	if (payload_len)
		memcpy(body, payload, payload_len);

	ipv4_pseudo_t pseudo;
	pseudo.src = htonl(dev->ipv4_addr);
	pseudo.dst = htonl(dst_ip);
	pseudo.zero = 0;
	pseudo.proto = IP_PROTO_TCP;
	pseudo.len = htons(tcp_len);
	tcp->checksum = htons(net_checksum2(&pseudo, sizeof(pseudo), tcp, tcp_len));

	/*
	 * Local delivery for connections to one of our own IPv4 addresses. This is
	 * required for tests such as connect(10.0.2.15:port) from the same host; ARP
	 * can resolve the address to our MAC, but the NIC will not normally loop that
	 * packet back into RX.
	 */
	if (dst_ip == dev->ipv4_addr) {
		dev->tx_packets++;
		net_receive_frame(dev, frame, frame_len);
		return 0;
	}

	return dev->send(dev, frame, frame_len);
}


int net_send_ipv4_tcp_window_opts(netdev_t *dev, const uint8_t dst_mac[6],
					  uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
					  uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
					  int include_mss, int include_wscale, uint8_t wscale,
					  int include_sack_permitted, const void *payload,
					  size_t payload_len)
{
	return net_send_ipv4_tcp_with_window_opts_impl(
		dev, dst_mac, dst_ip, src_port, dst_port, seq, ack, flags, window,
		include_mss, include_wscale, wscale, include_sack_permitted, payload,
		payload_len);
}

int net_send_ipv4_tcp_window(netdev_t *dev, const uint8_t dst_mac[6],
					  uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
					  uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
					  const void *payload, size_t payload_len)
{
	return net_send_ipv4_tcp_with_window_opts_impl(
		dev, dst_mac, dst_ip, src_port, dst_port, seq, ack, flags, window,
		(flags & TCP_SYN) != 0, 0, 0, 0, payload, payload_len);
}

int net_send_ipv4_tcp(netdev_t *dev, const uint8_t dst_mac[6],
					  uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
					  uint32_t seq, uint32_t ack, uint8_t flags,
					  const void *payload, size_t payload_len)
{
	return net_send_ipv4_tcp_with_window_opts_impl(
		dev, dst_mac, dst_ip, src_port, dst_port, seq, ack, flags, TCP_WINDOW,
		(flags & TCP_SYN) != 0, 0, 0, 0, payload, payload_len);
}
