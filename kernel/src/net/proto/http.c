#include "../stack.h"
#include <fs/vfs.h>
#include <lib/string.h>

static int ascii_digit(char c)
{
	return c >= '0' && c <= '9';
}

static int header_name_eq(const char *p, size_t len, const char *name)
{
	size_t name_len = strlen(name);
	return len == name_len && memcmp(p, name, name_len) == 0;
}

int net_http_parse(const char *buf, size_t len, net_http_response_t *out)
{
	if (!buf || !out || len < 12)
		return -EINVAL;
	if (memcmp(buf, "HTTP/", 5) != 0)
		return -EINVAL;

	size_t pos = 0;
	while (pos < len && buf[pos] != ' ')
		pos++;
	if (pos + 4 >= len || buf[pos] != ' ')
		return -EINVAL;
	pos++;
	if (!ascii_digit(buf[pos]) || !ascii_digit(buf[pos + 1]) ||
		!ascii_digit(buf[pos + 2]))
		return -EINVAL;

	memset(out, 0, sizeof(*out));
	out->status = (uint16_t)((buf[pos] - '0') * 100 +
							 (buf[pos + 1] - '0') * 10 +
							 (buf[pos + 2] - '0'));

	size_t header_end = 0;
	for (size_t i = 0; i + 3 < len; i++) {
		if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
			buf[i + 3] == '\n') {
			header_end = i + 4;
			break;
		}
	}
	if (!header_end)
		return -ENOENT;
	out->header_len = header_end;

	size_t line = 0;
	while (line < header_end) {
		size_t next = line;
		while (next + 1 < header_end &&
			   !(buf[next] == '\r' && buf[next + 1] == '\n'))
			next++;
		size_t colon = line;
		while (colon < next && buf[colon] != ':')
			colon++;
		if (colon < next && header_name_eq(buf + line, colon - line,
										   "Content-Length")) {
			size_t v = colon + 1;
			while (v < next && buf[v] == ' ')
				v++;
			size_t n = 0;
			while (v < next && ascii_digit(buf[v])) {
				n = n * 10 + (size_t)(buf[v] - '0');
				v++;
			}
			out->body_len = n;
			return 0;
		}
		line = next + 2;
	}

	out->body_len = len - header_end;
	return 0;
}

int net_http_get(const char *host, const char *path, char *buf, size_t len,
				 size_t *done, net_http_response_t *response,
				 uint64_t timeout_ms)
{
	if (done)
		*done = 0;
	if (!host || !path || !buf || len == 0)
		return -EINVAL;

	uint32_t ip = 0;
	int r = net_dns_resolve(host, timeout_ms, &ip);
	if (r != 0)
		return r;

	netdev_t *dev = net_route(ip, NULL);
	if (!dev)
		return -ENOENT;

	size_t got = 0;
	r = net_tcp_http_request(dev, ip, host, path, buf, len, &got, timeout_ms);
	if (done)
		*done = got;
	if (r != 0)
		return r;
	if (response)
		return net_http_parse(buf, got, response);
	return 0;
}

int net_http_get_dev(netdev_t *dev, const char *host, const char *path,
					 char *buf, size_t len, size_t *done,
					 net_http_response_t *response, uint64_t timeout_ms)
{
	if (done)
		*done = 0;
	if (!dev || !host || !path || !buf || len == 0)
		return -EINVAL;

	uint32_t ip = 0;
	int r = net_dns_resolve_dev(dev, host, timeout_ms, &ip);
	if (r != 0)
		return r;

	size_t got = 0;
	r = net_tcp_http_request(dev, ip, host, path, buf, len, &got, timeout_ms);
	if (done)
		*done = got;
	if (r != 0)
		return r;
	if (response)
		return net_http_parse(buf, got, response);
	return 0;
}
