#include <ping.h>
#include <netutil.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>

struct ping_packet {
	struct icmphdr hdr;
	unsigned char data[PING_DATA_SIZE];
};

#define PING_RX_SPIN_LIMIT 32

static unsigned short icmp_checksum(const void *buf, int len)
{
	const unsigned short *data = buf;
	unsigned int sum = 0;

	while (len > 1) {
		sum += *data++;
		len -= 2;
	}

	if (len == 1)
		sum += *(const unsigned char *)data;

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);

	return (unsigned short)~sum;
}

static long time_diff_us(const struct timeval *start, const struct timeval *end)
{
	return ((end->tv_sec - start->tv_sec) * 1000000L) +
		   (end->tv_usec - start->tv_usec);
}

static int ping_send(int s, const struct sockaddr_in *addr,
					 unsigned short ident, int seq, struct timeval *start)
{
	struct ping_packet pkt;

	memset(&pkt, 0, sizeof(pkt));

	pkt.hdr.type = ICMP_ECHO;
	pkt.hdr.code = 0;
	pkt.hdr.un.echo.id = htons(ident);
	pkt.hdr.un.echo.sequence = htons((unsigned short)seq);

	for (size_t i = 0; i < sizeof(pkt.data); i++)
		pkt.data[i] = (unsigned char)i;

	pkt.hdr.checksum = 0;
	pkt.hdr.checksum = icmp_checksum(&pkt, sizeof(pkt));

	gettimeofday(start, NULL);

	if (sendto(s, &pkt, sizeof(pkt), 0, (const struct sockaddr *)addr,
			   sizeof(*addr)) < 0) {
		print_errno("ping sendto");
		return -1;
	}

	return 0;
}

static int parse_icmp_reply(char *buf, ssize_t n, struct icmphdr **icmp_out,
							int *ttl_out)
{
	struct icmphdr *icmp;
	int ttl = -1;

	if (n >= (ssize_t)(sizeof(struct iphdr) + sizeof(struct icmphdr))) {
		struct iphdr *ip_hdr = (struct iphdr *)buf;
		size_t ip_hdr_len = ip_hdr->ihl * 4;

		if (ip_hdr_len < sizeof(struct iphdr))
			return -1;

		if (n < (ssize_t)(ip_hdr_len + sizeof(struct icmphdr)))
			return -1;

		ttl = ip_hdr->ttl;
		icmp = (struct icmphdr *)(buf + ip_hdr_len);
	} else if (n >= (ssize_t)sizeof(struct icmphdr)) {
		icmp = (struct icmphdr *)buf;
	} else {
		return -1;
	}

	*icmp_out = icmp;
	*ttl_out = ttl;
	return 0;
}

static int ping_recv(int s, unsigned short ident, int seq, long *rtt_us_out,
					 int *ttl_out, const struct timeval *start)
{
	char recv_buf[512];
	int spins = 0;

	while (spins++ < PING_RX_SPIN_LIMIT) {
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);
		struct timeval end;
		struct icmphdr *icmp = NULL;
		int ttl = -1;
		ssize_t n;

		n = recvfrom(s, recv_buf, sizeof(recv_buf), 0,
				     (struct sockaddr *)&from, &from_len);
		if (n < 0)
			return -1;

		gettimeofday(&end, NULL);

		if (time_diff_us(start, &end) > PING_TIMEOUT_SEC * 1000000L)
			return -1;

		if (parse_icmp_reply(recv_buf, n, &icmp, &ttl) < 0)
			continue;

		/* Raw sockets may also observe our own outgoing echo request. */
		if (icmp->type == ICMP_ECHO)
			continue;

		if (icmp->type == ICMP_ECHOREPLY && icmp->code == 0 &&
			ntohs(icmp->un.echo.id) == ident &&
			ntohs(icmp->un.echo.sequence) == seq) {
			if (rtt_us_out)
				*rtt_us_out = time_diff_us(start, &end);

			if (ttl_out)
				*ttl_out = ttl;

			return 0;
		}
	}

	return -1;
}

int ping(const char *host, int count)
{
	int s;
	int transmitted = 0;
	int received = 0;
	struct in_addr ip;
	struct sockaddr_in addr;
	struct timeval timeout;
	unsigned short ident;
	char ipstr[INET_ADDRSTRLEN];

	if (!host || count <= 0)
		return -1;

	if (resolve_ipv4(host, &ip) < 0) {
		printf("\033[1;31mping:\033[0m failed to resolve %s\n", host);
		return -1;
	}

	if (inet_ntop(AF_INET, &ip, ipstr, sizeof(ipstr)) == NULL) {
		print_errno("ping inet_ntop");
		return -1;
	}

	s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (s < 0) {
		print_errno("ping socket");
		return -1;
	}

	timeout.tv_sec = PING_TIMEOUT_SEC;
	timeout.tv_usec = 0;

	if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
		print_errno("ping setsockopt SO_RCVTIMEO");
		close(s);
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr = ip;

	ident = (unsigned short)(getpid() & 0xffff);

	printf("\033[1;34mping:\033[0m PING %s (%s): %d data bytes\n", host, ipstr,
		   PING_DATA_SIZE);

	for (int seq = 1; seq <= count; seq++) {
		struct timeval start;
		long rtt_us = 0;
		int ttl = -1;

		transmitted++;

		if (ping_send(s, &addr, ident, seq, &start) < 0) {
			printf("Request failed for icmp_seq %d\n", seq);
			continue;
		}

		if (ping_recv(s, ident, seq, &rtt_us, &ttl, &start) == 0) {
			received++;

			if (ttl >= 0) {
				printf(
					"%d bytes from %s: icmp_seq=%d ttl=%d time=%ld.%03ld ms\n",
					PING_DATA_SIZE + 8, ipstr, seq, ttl, rtt_us / 1000,
					rtt_us % 1000);
			} else {
				printf("%d bytes from %s: icmp_seq=%d time=%ld.%03ld ms\n",
					   PING_DATA_SIZE + 8, ipstr, seq, rtt_us / 1000,
					   rtt_us % 1000);
			}
		} else {
			printf("Request timeout for icmp_seq %d\n", seq);
		}
	}

	close(s);

	printf("--- %s ping statistics ---\n", host);
	printf("%d packets transmitted, %d packets received, %d%% packet loss\n",
		   transmitted, received,
		   transmitted == 0 ? 100 :
							  ((transmitted - received) * 100) / transmitted);

	return received > 0 ? 0 : -1;
}