#ifndef _LYR_NET_STACK_H
#define _LYR_NET_STACK_H

#include <net/net.h>
#include <stddef.h>
#include <stdint.h>

#define ETH_P_IP 0x0800
#define ETH_P_ARP 0x0806
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_PSH 0x08
#define TCP_ACK 0x10

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
	uint16_t src_port;
	uint16_t dst_port;
	uint32_t seq;
	uint32_t ack;
	uint8_t data_off;
	uint8_t flags;
	uint16_t window;
	uint16_t checksum;
	uint16_t urgent;
} tcp_hdr_t;

typedef struct __attribute__((packed)) {
	uint32_t src;
	uint32_t dst;
	uint8_t zero;
	uint8_t proto;
	uint16_t len;
} ipv4_pseudo_t;

static inline uint16_t net_bswap16(uint16_t v)
{
	return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint32_t net_bswap32(uint32_t v)
{
	return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
		   ((v & 0x00ff0000u) >> 8) | ((v & 0xff000000u) >> 24);
}

#define htons(v) net_bswap16((uint16_t)(v))
#define ntohs(v) net_bswap16((uint16_t)(v))
#define htonl(v) net_bswap32((uint32_t)(v))
#define ntohl(v) net_bswap32((uint32_t)(v))

uint64_t net_timeout_ticks(uint64_t timeout_ms);
void net_poll_until(netdev_t *dev, uint64_t until_tick, int *flag);
uint16_t net_checksum(const void *data, size_t len);
uint16_t net_checksum2(const void *a, size_t a_len, const void *b,
					   size_t b_len);

int net_send_ipv4_udp(netdev_t *dev, const uint8_t dst_mac[6],
					  uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
					  uint16_t dst_port, const void *payload,
					  size_t payload_len);
int net_send_ipv4_icmp(netdev_t *dev, const uint8_t dst_mac[6],
					   uint32_t src_ip, uint32_t dst_ip,
					   const void *payload, size_t payload_len);
int net_send_ipv4_tcp(netdev_t *dev, const uint8_t dst_mac[6],
					  uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
					  uint32_t seq, uint32_t ack, uint8_t flags,
					  const void *payload, size_t payload_len);
int net_send_ipv4_tcp_window(netdev_t *dev, const uint8_t dst_mac[6],
					  uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
					  uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
					  const void *payload, size_t payload_len);
int net_send_ipv4_tcp_window_opts(netdev_t *dev, const uint8_t dst_mac[6],
					  uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
					  uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
					  int include_mss, int include_wscale, uint8_t wscale,
					  int include_sack_permitted, const void *payload,
					  size_t payload_len);

int net_arp_resolve(netdev_t *dev, uint32_t target_ip, uint64_t timeout_ms,
					uint8_t out_mac[NET_ETH_ALEN]);
size_t net_arp_cache_count(netdev_t *dev);
void net_arp_receive(netdev_t *dev, const arp_pkt_t *arp);
void net_dhcp_receive(netdev_t *dev, const udp_hdr_t *udp, size_t udp_len);
void net_dns_receive(netdev_t *dev, const udp_hdr_t *udp, size_t udp_len);
void net_icmp_receive(netdev_t *dev, const ipv4_hdr_t *ip, size_t ihl,
					  size_t ip_len);
void net_tcp_receive(netdev_t *dev, const uint8_t src_mac[NET_ETH_ALEN],
					 const ipv4_hdr_t *ip, size_t ihl, size_t ip_len);
void net_socket_udp_receive(netdev_t *dev, const ipv4_hdr_t *ip,
							const udp_hdr_t *udp, size_t udp_len);
void net_socket_raw_icmp_receive(netdev_t *dev, const ipv4_hdr_t *ip,
								 size_t ihl, size_t ip_len);
int net_tcp_http_request(netdev_t *dev, uint32_t dst_ip, const char *host,
						 const char *path, char *buf, size_t len,
						 size_t *done, uint64_t timeout_ms);

#endif /* _LYR_NET_STACK_H */
