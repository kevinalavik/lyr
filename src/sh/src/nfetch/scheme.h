#ifndef SH_NFETCH_SCHEME_H
#define SH_NFETCH_SCHEME_H

#include <stddef.h>
#include <sh.h>

typedef struct nfetch_url {
	char *scheme;
	char *host;
	char *port;
	char *path;
} nfetch_url_t;

typedef struct nfetch_opts {
	const char *url;
	const char *outfile;
	const char *send_data;
	const char *username;
	const char *password;
	int verbose;
	int include_headers;
	int timeout_sec;
	int tcp_crlf;
	int tcp_cr;
	int tcp_no_newline;
} nfetch_opts_t;

void nfetch_url_free(nfetch_url_t *u);
int nfetch_parse_url(const char *input, nfetch_url_t *out);
int nfetch_connect_tcp(const char *host, const char *port, int verbose);
int nfetch_connect_tcp_timeout(const char *host, const char *port, int verbose,
					   int timeout_sec);
int nfetch_copy_response(int s, int outfd, int timeout_sec);
int nfetch_copy_http_body(int s, int outfd, int timeout_sec);
int nfetch_drain_response_once(int s, int outfd, int timeout_sec);
void nfetch_set_recv_timeout(int s, int timeout_sec);
int nfetch_write_all(int fd, const void *buf, size_t len);
int nfetch_open_output(const char *path);
int nfetch_sock_write_all(int fd, const void *buf, size_t len);
char *nfetch_decode_escapes(const char *s, size_t *len_out);

int nfetch_scheme_http(const nfetch_opts_t *opts, const nfetch_url_t *url);
int nfetch_scheme_tcp(const nfetch_opts_t *opts, const nfetch_url_t *url);
int nfetch_scheme_ftp(const nfetch_opts_t *opts, const nfetch_url_t *url);

int nfetch_run(int argc, char **argv);

#endif