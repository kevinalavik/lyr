#ifndef _LYR_NET_NET_H
#define _LYR_NET_NET_H

#include <stddef.h>
#include <stdint.h>

#define NETDEV_NAME_MAX 15
#define NET_ETH_ALEN 6
#define NET_MTU 1500
#define NET_ETH_FRAME_MAX 1518
#define NETDEV_RX_QUEUE_LEN 16
#define NET_HTTP_HEADER_MAX 1024

typedef struct netdev netdev_t;

typedef struct {
	uint32_t src_ip;
	uint16_t seq;
	uint16_t bytes;
	uint8_t ttl;
	uint64_t time_ms;
} net_ping_result_t;

typedef struct {
	uint16_t status;
	size_t header_len;
	size_t body_len;
} net_http_response_t;

typedef int (*netdev_send_t)(netdev_t *dev, const void *frame, size_t len);
typedef int (*netdev_poll_t)(netdev_t *dev);

struct netdev {
	char name[NETDEV_NAME_MAX + 1];
	uint8_t mac[NET_ETH_ALEN];
	uint32_t ipv4_addr;
	uint32_t ipv4_gateway;
	uint32_t ipv4_netmask;
	uint32_t dhcp_server;
	uint32_t dns_server;
	uint16_t mtu;
	int dhcp_configured;
	netdev_send_t send;
	netdev_poll_t poll;
	void *driver_data;
	uint8_t *rx_queue;
	uint16_t rx_len[NETDEV_RX_QUEUE_LEN];
	size_t rx_head;
	size_t rx_tail;
	size_t rx_count;
	uint64_t rx_dropped;
	uint64_t tx_packets;
	uint64_t rx_packets;
	netdev_t *next;
};

int net_init(void);
int netdev_register(netdev_t *dev);
void net_receive_frame(netdev_t *dev, const void *frame, size_t len);
int net_dhcp_configure(uint64_t timeout_ms);
int net_ping_echo(uint32_t dst_ip, uint16_t ident, uint16_t seq,
				  uint64_t timeout_ms, net_ping_result_t *result);
int net_ping(uint32_t dst_ip, uint16_t ident, uint16_t seq, uint64_t timeout_ms);
int net_dns_resolve(const char *name, uint64_t timeout_ms, uint32_t *out_ip);
int net_http_parse(const char *buf, size_t len, net_http_response_t *out);
int net_http_get(const char *host, const char *path, char *buf, size_t len,
				 size_t *done, net_http_response_t *response,
				 uint64_t timeout_ms);
uint32_t net_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
void net_ipv4_format(uint32_t ip, char *out, size_t len);
uint32_t net_default_ipv4(void);
uint32_t net_default_gateway(void);

#endif /* _LYR_NET_NET_H */
