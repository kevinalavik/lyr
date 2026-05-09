#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sh.h>
#include "scheme.h"

int nfetch_run(int argc, char **argv)
{
	nfetch_opts_t opts;
	memset(&opts, 0, sizeof(opts));

	opts.timeout_sec = 5;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			puts("usage:");
			puts("  nfetch [options] URL");
			puts("");
			puts("examples:");
			puts("  nfetch example.com");
			puts("  nfetch http://example.com/");
			puts("  nfetch -o page.txt http://example.com/");
			puts("  nfetch -i http://example.com/     # include HTTP response headers");
			puts("  nfetch tcp://tcpbin.com:4242");
			puts("  nfetch tcp://tcpbin.com:4242 -s 'hello\\n'");
			puts("  nfetch ftp://ftp.example.com/pub/file.txt -o file.txt");
			puts("  nfetch ftp://ftp.example.com -u ftp -p mail@example.com");
			puts("  nfetch ftp://ftp.example.com/       # FTP command REPL");
			puts(
				"  nfetch tcp://example.com:80 -s 'GET / HTTP/1.0\\r\\nHost: example.com\\r\\n\\r\\n'");
			puts("");
			puts("schemes:");
			puts("  http://HOST[:PORT][/PATH]   HTTP GET only, no TLS");
			puts("  ftp://HOST[:PORT][/PATH]    standard FTP, USER/PASS login, PASV");
			puts(
				"  tcp://HOST[:PORT]           raw TCP; REPL if -s is omitted");
			puts("");
			puts("options:");
			puts("  -o FILE        write output to FILE");
			puts("  -u USER        FTP username");
			puts("  -p PASS        FTP password");
			puts(
				"  -s DATA        send DATA first; supports \\n, \\r, \\t, \\0, \\\\, \\xHH");
			puts("  -t SEC         timeout in seconds, default 5; FTP uses it for connects only");
			puts("  -i, --include-headers");
			puts("                 include HTTP response status line and headers in output");
			puts("  -v             verbose diagnostics");
			puts("  FTP downloads to files show progress on stderr");
			puts("  --lf           TCP REPL line ending: LF, default");
			puts("  --crlf         TCP REPL line ending: CRLF");
			puts("  --cr           TCP REPL line ending: CR");
			puts("  -N             TCP REPL sends no automatic line ending");
			return 0;
		}

		if (strcmp(argv[i], "-o") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "nfetch: -o requires a file\n");
				return 2;
			}

			opts.outfile = argv[++i];
			continue;
		}

		if (strcmp(argv[i], "-s") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "nfetch: -s requires data\n");
				return 2;
			}

			opts.send_data = argv[++i];
			continue;
		}

		if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--user") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "nfetch: %s requires a username\n", argv[i]);
				return 2;
			}

			opts.username = argv[++i];
			continue;
		}

		if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--password") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "nfetch: %s requires a password\n", argv[i]);
				return 2;
			}

			opts.password = argv[++i];
			continue;
		}

		if (strcmp(argv[i], "-t") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "nfetch: -t requires seconds\n");
				return 2;
			}

			opts.timeout_sec = atoi(argv[++i]);

			if (opts.timeout_sec <= 0)
				opts.timeout_sec = 1;

			continue;
		}

		if (strcmp(argv[i], "--crlf") == 0) {
			opts.tcp_crlf = 1;
			opts.tcp_cr = 0;
			opts.tcp_no_newline = 0;
			continue;
		}

		if (strcmp(argv[i], "--cr") == 0) {
			opts.tcp_cr = 1;
			opts.tcp_crlf = 0;
			opts.tcp_no_newline = 0;
			continue;
		}

		if (strcmp(argv[i], "--lf") == 0) {
			opts.tcp_cr = 0;
			opts.tcp_crlf = 0;
			opts.tcp_no_newline = 0;
			continue;
		}

		if (strcmp(argv[i], "-N") == 0 ||
			strcmp(argv[i], "--no-newline") == 0) {
			opts.tcp_no_newline = 1;
			opts.tcp_cr = 0;
			opts.tcp_crlf = 0;
			continue;
		}

		if (strcmp(argv[i], "-i") == 0 ||
			strcmp(argv[i], "--include-headers") == 0) {
			opts.include_headers = 1;
			continue;
		}

		if (strcmp(argv[i], "-v") == 0) {
			opts.verbose = 1;
			continue;
		}

		if (argv[i][0] == '-' && argv[i][1]) {
			fprintf(stderr, "nfetch: unsupported option: %s\n", argv[i]);
			return 2;
		}

		if (opts.url) {
			fprintf(stderr, "nfetch: too many URLs\n");
			return 2;
		}

		opts.url = argv[i];
	}

	if (!opts.url) {
		fprintf(stderr, "nfetch: missing URL\n");
		return 2;
	}

	nfetch_url_t url;

	if (nfetch_parse_url(opts.url, &url) < 0) {
		nfetch_url_free(&url);
		return 2;
	}

	if ((opts.username || opts.password) && strcmp(url.scheme, "ftp") != 0) {
		fprintf(stderr,
				"nfetch: -u/-p are only valid for schemes that support login: %s does not\n",
				url.scheme);
		nfetch_url_free(&url);
		return 2;
	}

	int status;

	if (strcmp(url.scheme, "http") == 0)
		status = nfetch_scheme_http(&opts, &url);
	else if (strcmp(url.scheme, "tcp") == 0)
		status = nfetch_scheme_tcp(&opts, &url);
	else if (strcmp(url.scheme, "ftp") == 0)
		status = nfetch_scheme_ftp(&opts, &url);
	else {
		fprintf(stderr, "nfetch: unsupported scheme: %s\n", url.scheme);
		status = 2;
	}

	nfetch_url_free(&url);
	return status;
}