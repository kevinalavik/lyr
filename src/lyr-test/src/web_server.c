#include <web_server.h>
#include <netutil.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HTTP_REQ_SIZE 512
#define HTTP_RESP_SIZE 1024
#define HTTP_BACKLOG 8

static int parse_request_line(const char *req, struct http_request *out)
{
	char version[16];

	memset(out, 0, sizeof(*out));

	if (sscanf(req, "%7s %127s %15s", out->method, out->path, version) != 3)
		return -1;

	if (strncmp(version, "HTTP/", 5) != 0)
		return -1;

	return 0;
}

static const struct http_route *find_route(const struct http_route *routes,
					   size_t route_count,
					   const struct http_request *req)
{
	for (size_t i = 0; i < route_count; i++) {
		if (!routes[i].method || !routes[i].path || !routes[i].handler)
			continue;

		if (strcmp(routes[i].method, req->method) == 0 &&
		    strcmp(routes[i].path, req->path) == 0) {
			return &routes[i];
		}
	}

	return NULL;
}

void http_response_text(struct http_response *res,
			       int status,
			       const char *reason,
			       const char *body)
{
	res->status = status;
	res->reason = reason;
	res->content_type = "text/plain";
	res->body = body;
}

void http_response_not_found(struct http_response *res)
{
	http_response_text(res, 404, "Not Found", "404 not found\n");
}

static int send_response(int c, const struct http_response *res)
{
	char header[HTTP_RESP_SIZE];
	const char *body = res->body ? res->body : "";
	const char *reason = res->reason ? res->reason : "OK";
	const char *type = res->content_type ? res->content_type : "text/plain";
	int status = res->status ? res->status : 200;
	int n;

	n = snprintf(header, sizeof(header),
		     "HTTP/1.0 %d %s\r\n"
		     "Server: lyrOS-userspace\r\n"
		     "Content-Type: %s\r\n"
		     "Content-Length: %d\r\n"
		     "Connection: close\r\n"
		     "\r\n",
		     status, reason, type, (int)strlen(body));

	if (n < 0 || (size_t)n >= sizeof(header))
		return -1;

	if (write_all(c, header, (size_t)n, "web write header") < 0)
		return -1;

	return write_all(c, body, strlen(body), "web write body");
}

static void serve_client(int c,
			 const struct http_route *routes,
			 size_t route_count)
{
	char raw[HTTP_REQ_SIZE];
	struct http_request req;
	struct http_response res;
	const struct http_route *route;
	ssize_t n;

	n = read(c, raw, sizeof(raw) - 1);
	if (n < 0) {
		print_errno("web read");
		return;
	}

	if (n == 0)
		return;

	raw[n] = 0;

	if (parse_request_line(raw, &req) < 0) {
		http_response_text(&res, 400, "Bad Request", "400 bad request\n");
		(void)send_response(c, &res);
		return;
	}

	printf("\033[1;34mweb:\033[0m %s %s\n", req.method, req.path);

	route = find_route(routes, route_count, &req);
	if (!route) {
		http_response_not_found(&res);
		(void)send_response(c, &res);
		return;
	}

	memset(&res, 0, sizeof(res));
	route->handler(&req, &res, route->ctx);

	if (send_response(c, &res) == 0)
		printf("\033[1;35mweb:\033[0m served %s %s\n", req.method, req.path);
}

int http_server_listen(unsigned short port,
		       const struct http_route *routes,
		       size_t route_count)
{
	int s;
	struct sockaddr_in addr;

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		print_errno("web socket");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = inet_addr("0.0.0.0");

	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		print_errno("web bind");
		close(s);
		return -1;
	}

	if (listen(s, HTTP_BACKLOG) < 0) {
		print_errno("web listen");
		close(s);
		return -1;
	}

	printf("\033[1;32mweb:\033[0m listening on 0.0.0.0:%u\n", port);

	for (;;) {
		int c = accept(s, NULL, NULL);

		if (c < 0) {
			if (errno == EINTR)
				continue;

			print_errno("web accept");
			continue;
		}

		serve_client(c, routes, route_count);
		close(c);
	}

	close(s);
	return 0;
}
