#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stddef.h>

struct http_request {
	char method[8];
	char path[128];
};

struct http_response {
	int status;
	const char *reason;
	const char *content_type;
	const char *body;
};

typedef void (*http_handler_t)(const struct http_request *req,
				      struct http_response *res,
				      void *ctx);

struct http_route {
	const char *method;
	const char *path;
	http_handler_t handler;
	void *ctx;
};

void http_response_text(struct http_response *res,
			       int status,
			       const char *reason,
			       const char *body);

void http_response_not_found(struct http_response *res);

int http_server_listen(unsigned short port,
		       const struct http_route *routes,
		       size_t route_count);

#endif
