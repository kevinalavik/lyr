#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include <lyr/mount.h>

#define PING_PKT_SIZE 64
#define PING_TIMEOUT_SEC 1

struct ping_packet {
	struct icmphdr hdr;
	char payload[PING_PKT_SIZE - sizeof(struct icmphdr)];
};

static unsigned short icmp_checksum(void *buf, int len)
{
	unsigned short *data = buf;
	unsigned int sum = 0;

	while (len > 1) {
		sum += *data++;
		len -= 2;
	}

	if (len == 1)
		sum += *(unsigned char *)data;

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);

	return (unsigned short)(~sum);
}

static int ping_ip_once(const char *ip)
{
	int sockfd;
	struct sockaddr_in addr;
	struct ping_packet pkt;
	unsigned short ident;
	unsigned short seq = 1;

	sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (sockfd < 0) {
		fprintf(stderr, "init: failed to create ICMP socket: %s\n",
				strerror(errno));
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;

	if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
		fprintf(stderr, "init: invalid IPv4 address: %s\n", ip);
		close(sockfd);
		return -1;
	}

	memset(&pkt, 0, sizeof(pkt));

	ident = getpid() & 0xffff;

	pkt.hdr.type = ICMP_ECHO;
	pkt.hdr.code = 0;
	pkt.hdr.un.echo.id = ident;
	pkt.hdr.un.echo.sequence = seq;

	for (size_t i = 0; i < sizeof(pkt.payload); i++)
		pkt.payload[i] = (char)i;

	pkt.hdr.checksum = 0;
	pkt.hdr.checksum = icmp_checksum(&pkt, sizeof(pkt));

	if (sendto(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&addr,
			   sizeof(addr)) < 0) {
		fprintf(stderr, "init: failed to send ICMP echo to %s: %s\n", ip,
				strerror(errno));
		close(sockfd);
		return -1;
	}

	close(sockfd);
	return 0;
}

static int get_field(const char *line, const char *key, char *out,
					 size_t out_size)
{
	const char *pos;
	const char *end;
	size_t len;

	pos = strstr(line, key);
	if (!pos)
		return -1;

	pos += strlen(key);
	end = pos;

	while (*end && *end != ' ' && *end != '\n' && *end != '\t')
		end++;

	len = (size_t)(end - pos);

	if (len == 0 || len >= out_size)
		return -1;

	memcpy(out, pos, len);
	out[len] = '\0';

	return 0;
}

static int check_ifaces(void)
{
	const char *iface_list = "/dev/net/devices";
	FILE *fp;
	char line[512];
	int found_working_iface = -1;

	fp = fopen(iface_list, "r");
	if (!fp) {
		fprintf(stderr, "init: failed to open %s: %s\n", iface_list,
				strerror(errno));
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		char iface[64];
		char link[16];
		char ip[64];
		char gateway[64];
		char target[64];

		if (sscanf(line, "%63s", iface) != 1)
			continue;

		if (get_field(line, "link=", link, sizeof(link)) < 0)
			continue;

		if (strcmp(link, "up") != 0)
			continue;

		memset(ip, 0, sizeof(ip));
		memset(gateway, 0, sizeof(gateway));
		memset(target, 0, sizeof(target));

		get_field(line, "ip=", ip, sizeof(ip));
		get_field(line, "gateway=", gateway, sizeof(gateway));

		if (strstr(line, "loopback") || strcmp(iface, "lo") == 0) {
			strcpy(target, "127.0.0.1");
		}

		else if (gateway[0] && strcmp(gateway, "0.0.0.0") != 0) {
			strncpy(target, gateway, sizeof(target));
		}

		else {
			continue;
		}

		if (ping_ip_once(target) == 0) {
			printf("init: iface %s has working ICMP target %s\n", iface,
				   target);
			found_working_iface = 0;
		}
	}

	fclose(fp);
	return found_working_iface;
}

static int mount_or_report(const char *source, const char *target,
						   const char *filesystem, unsigned long flags,
						   const void *data)
{
	if (mount(source, target, filesystem, flags, data) < 0) {
		fprintf(stderr, "init: failed to mount %s at %s as %s: %s\n", source,
				target, filesystem, strerror(errno));
		return -1;
	}

	printf("init: mounted %s at %s\n", filesystem, target);
	return 0;
}

int main(void)
{
	printf("init: Welcome to lyrOS v1.0 (mlibc)\n");

	if (mount_or_report("devfs", "/dev", "devfs", 0, NULL) < 0) {
		return 1;
	}

	if (mount_or_report("tmpfs", "/tmp", "tmpfs", 0, NULL) < 0) {
		return 1;
	}

	check_ifaces();

	static char path[] = "/usr/bin/lyr-test";
	static char *const argv[] = { path, NULL };
	static char *const envp[] = { NULL };

	printf("init: executing %s\n", path);
	execve(path, argv, envp);

	fprintf(stderr, "init: execve(%s) failed: %s\n", path, strerror(errno));
	return 127;
}