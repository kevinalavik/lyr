#include "../stack.h"
#include <errno.h>
#include <stddef.h>

#define NET_IPV4_PROTOCOL_MAX 8
#define NET_UDP_HANDLER_MAX 8

static const net_ipv4_protocol_ops_t *ipv4_protocols[NET_IPV4_PROTOCOL_MAX];
static const net_udp_handler_ops_t *udp_handlers[NET_UDP_HANDLER_MAX];
static int net_proto_ready;

static int register_ops(const void **slots, size_t cap, const void *ops,
						 uint8_t key, size_t key_off)
{
	for (size_t i = 0; i < cap; i++) {
		if (!slots[i])
			continue;
		const uint8_t *cur = slots[i];
		if (slots[i] == ops || cur[key_off] == key)
			return -EEXIST;
	}

	for (size_t i = 0; i < cap; i++) {
		if (!slots[i]) {
			slots[i] = ops;
			return 0;
		}
	}

	return -ENOMEM;
}

int net_ipv4_register_protocol(const net_ipv4_protocol_ops_t *ops)
{
	if (!ops || !ops->name || !ops->receive || ops->protocol == 0)
		return -EINVAL;

	return register_ops((const void **)ipv4_protocols, NET_IPV4_PROTOCOL_MAX,
						ops, ops->protocol,
						offsetof(net_ipv4_protocol_ops_t, protocol));
}

int net_udp_register_handler(const net_udp_handler_ops_t *ops)
{
	if (!ops || !ops->name || !ops->receive)
		return -EINVAL;

	for (size_t i = 0; i < NET_UDP_HANDLER_MAX; i++) {
		if (udp_handlers[i] == ops)
			return -EEXIST;
	}

	for (size_t i = 0; i < NET_UDP_HANDLER_MAX; i++) {
		if (!udp_handlers[i]) {
			udp_handlers[i] = ops;
			return 0;
		}
	}

	return -ENOMEM;
}

int net_ipv4_receive(const net_ipv4_rx_info_t *rx)
{
	if (!rx || !rx->ip)
		return -EINVAL;

	for (size_t i = 0; i < NET_IPV4_PROTOCOL_MAX; i++) {
		const net_ipv4_protocol_ops_t *ops = ipv4_protocols[i];
		if (!ops || ops->protocol != rx->ip->proto)
			continue;
		return ops->receive(rx, ops->ctx);
	}

	return -ENOENT;
}

int net_proto_init(void)
{
	if (net_proto_ready)
		return 0;

	int r = net_udp_init();
	if (r != 0 && r != -EEXIST)
		return r;

	r = net_dhcp_init();
	if (r != 0 && r != -EEXIST)
		return r;

	r = net_dns_init();
	if (r != 0 && r != -EEXIST)
		return r;

	r = net_icmp_init();
	if (r != 0 && r != -EEXIST)
		return r;

	r = net_tcp_init();
	if (r != 0 && r != -EEXIST)
		return r;

	net_proto_ready = 1;
	return 0;
}

int net_udp_deliver(const net_udp_dgram_t *dgram)
{
	if (!dgram)
		return -EINVAL;

	int handled = 0;
	for (size_t i = 0; i < NET_UDP_HANDLER_MAX; i++) {
		const net_udp_handler_ops_t *ops = udp_handlers[i];
		if (!ops)
			continue;
		int r = ops->receive(dgram, ops->ctx);
		if (r > 0)
			handled = 1;
	}

	return handled ? 0 : -ENOENT;
}
