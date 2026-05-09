#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "scheme.h"

int nfetch_scheme_http(const nfetch_opts_t *opts, const nfetch_url_t *url)
{
	int outfd = nfetch_open_output(opts->outfile);

	if (outfd < 0)
		return 1;

	int s = nfetch_connect_tcp(url->host, url->port, opts->verbose);

	if (s < 0) {
		if (outfd != STDOUT_FILENO)
			close(outfd);
		return 1;
	}

	char host_hdr[512];

	if (strcmp(url->port, "80") == 0)
		snprintf(host_hdr, sizeof(host_hdr), "%s", url->host);
	else
		snprintf(host_hdr, sizeof(host_hdr), "%s:%s", url->host, url->port);

	char req[4096];

	int n = snprintf(req, sizeof(req),
					 "GET %s HTTP/1.0\r\n"
					 "Host: %s\r\n"
					 "User-Agent: nfetch/0.1\r\n"
					 "Accept: */*\r\n"
					 "Connection: close\r\n"
					 "\r\n",
					 url->path, host_hdr);

	if (n < 0 || (size_t)n >= sizeof(req)) {
		fprintf(stderr, "nfetch: request too large\n");
		close(s);
		if (outfd != STDOUT_FILENO)
			close(outfd);
		return 1;
	}

	if (opts->verbose) {
		fprintf(stderr, "nfetch: request:\n");
		fprintf(stderr, "%s", req);
	}

	if (nfetch_sock_write_all(s, req, (size_t)n) < 0) {
		fprintf(stderr, "nfetch: write: %s\n", strerror(errno));
		close(s);
		if (outfd != STDOUT_FILENO)
			close(outfd);
		return 1;
	}

	if (opts->verbose)
		fprintf(stderr, "nfetch: sent %d bytes\n", n);

	int status;

	if (opts->include_headers)
		status = nfetch_copy_response(s, outfd, opts->timeout_sec);
	else
		status = nfetch_copy_http_body(s, outfd, opts->timeout_sec);

	close(s);

	if (outfd != STDOUT_FILENO)
		close(outfd);

	return status;
}