#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "scheme.h"

static int nfetch_tcp_repl(const nfetch_opts_t *opts, int s, int outfd)
{
	if (opts->verbose) {
		fprintf(stderr, "nfetch: interactive tcp mode\n");

		if (opts->tcp_no_newline)
			fprintf(stderr, "nfetch: line ending: none\n");
		else if (opts->tcp_crlf)
			fprintf(stderr, "nfetch: line ending: CRLF\n");
		else if (opts->tcp_cr)
			fprintf(stderr, "nfetch: line ending: CR\n");
		else
			fprintf(stderr, "nfetch: line ending: LF\n");
	}

	for (;;) {
		fputs("tcp> ", stderr);
		fflush(stderr);

		char *line = sh_read_line(stdin);

		if (!line)
			break;

		size_t len = strlen(line);

		if (len && line[len - 1] == '\n') {
			line[len - 1] = '\0';
			len--;
		}

		if (strcmp(line, ".") == 0 || strcmp(line, "/quit") == 0 ||
			strcmp(line, "/exit") == 0) {
			free(line);
			break;
		}

		if (len > 0 && nfetch_sock_write_all(s, line, len) < 0) {
			fprintf(stderr, "nfetch: write: %s\n", strerror(errno));
			free(line);
			return 1;
		}

		if (!opts->tcp_no_newline) {
			const char *eol = "\n";
			size_t eol_len = 1;

			if (opts->tcp_crlf) {
				eol = "\r\n";
				eol_len = 2;
			} else if (opts->tcp_cr) {
				eol = "\r";
				eol_len = 1;
			}

			if (nfetch_sock_write_all(s, eol, eol_len) < 0) {
				fprintf(stderr, "nfetch: write: %s\n", strerror(errno));
				free(line);
				return 1;
			}
		}

		free(line);

		int r = nfetch_drain_response_once(s, outfd, opts->timeout_sec);

		if (r == 2)
			break;
	}

	return 0;
}

int nfetch_scheme_tcp(const nfetch_opts_t *opts, const nfetch_url_t *url)
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

	int status = 0;

	if (opts->send_data) {
		size_t send_len = 0;
		char *send_buf = nfetch_decode_escapes(opts->send_data, &send_len);

		if (opts->verbose)
			fprintf(stderr, "nfetch: sending %lu bytes\n",
					(unsigned long)send_len);

		if (send_len > 0 && nfetch_sock_write_all(s, send_buf, send_len) < 0) {
			fprintf(stderr, "nfetch: write: %s\n", strerror(errno));
			free(send_buf);
			close(s);
			if (outfd != STDOUT_FILENO)
				close(outfd);
			return 1;
		}

		if (opts->verbose)
			fprintf(stderr, "nfetch: sent %lu bytes\n",
					(unsigned long)send_len);

		free(send_buf);

		status = nfetch_copy_response(s, outfd, opts->timeout_sec);
	} else {
		status = nfetch_tcp_repl(opts, s, outfd);
	}

	close(s);

	if (outfd != STDOUT_FILENO)
		close(outfd);

	return status;
}