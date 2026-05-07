#include <nist_time.h>
#include <netutil.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char *trim_leading_crlf(char *s)
{
	while (*s == '\r' || *s == '\n')
		s++;

	return s;
}

static int parse_nist_daytime(const char *line, struct nist_time *out)
{
	int yy;
	char zone[sizeof(out->zone)];
	char sync[8];

	memset(out, 0, sizeof(*out));
	memset(zone, 0, sizeof(zone));
	memset(sync, 0, sizeof(sync));

	if (sscanf(line, "%d %d-%d-%d %d:%d:%d %d %d %d %lf %31s %7s", &out->mjd,
			   &yy, &out->month, &out->day, &out->hour, &out->minute,
			   &out->second, &out->dst, &out->leap, &out->health, &out->ut1,
			   zone, sync) != 13) {
		return -1;
	}

	out->year = 2000 + yy;
	strncpy(out->zone, zone, sizeof(out->zone) - 1);
	out->zone[sizeof(out->zone) - 1] = 0;
	out->sync = sync[0];

	return 0;
}

int get_nist_time(struct nist_time *out)
{
	for (int attempt = 0; attempt < 5; attempt++) {
		int s;
		char buf[512];
		char *line;
		int n;

		s = tcp_connect_host("time.nist.gov", 13);
		if (s < 0)
			continue;

		n = read_some_text(s, buf, sizeof(buf));
		close(s);

		if (n < 0)
			continue;

		line = trim_leading_crlf(buf);
		if (*line == 0)
			continue;

		if (parse_nist_daytime(line, out) == 0)
			return 0;
	}

	return -1;
}

void print_nist_time(const struct nist_time *t)
{
	printf("\033[1;32mtime:\033[0m synced: %04d-%02d-%02d %02d:%02d:%02d %s\n",
		   t->year, t->month, t->day, t->hour, t->minute, t->second, t->zone);
}