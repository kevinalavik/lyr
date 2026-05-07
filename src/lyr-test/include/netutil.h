#ifndef NETUTIL_H
#define NETUTIL_H

#include <stddef.h>
#include <netinet/in.h>

void print_errno(const char *where);
int write_all(int fd, const void *buf, size_t len, const char *where);
int read_some_text(int fd, char *buf, size_t len);
int resolve_ipv4(const char *host, struct in_addr *out);
int tcp_connect_ipv4(struct in_addr ip, unsigned short port);
int tcp_connect_host(const char *host, unsigned short port);

#endif