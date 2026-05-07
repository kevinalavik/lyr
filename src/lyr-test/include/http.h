#ifndef HTTP_H
#define HTTP_H

#include <netinet/in.h>

int http_get_ip_port(struct in_addr ip, unsigned short port, const char *host,
					 const char *path);

#endif