#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sh.h>
#include <builtin.h>

int sh_builtin_echo(int argc, char **argv)
{
	int newline = 1;
	int i = 1;

	if (i < argc && strcmp(argv[i], "-n") == 0) {
		newline = 0;
		i++;
	}

	for (; i < argc; i++) {
		if (i > (newline ? 1 : 2))
			putchar(' ');
		fputs(argv[i], stdout);
	}

	if (newline)
		putchar('\n');

	return 0;
}

static int printf_escape_char(char c)
{
	switch (c) {
	case 'n':
		return '\n';
	case 'r':
		return '\r';
	case 't':
		return '\t';
	case 'b':
		return '\b';
	case 'a':
		return '\a';
	case '\\':
		return '\\';
	case '0':
		return '\0';
	default:
		return c;
	}
}

int sh_builtin_printf(int argc, char **argv)
{
	if (argc < 2)
		return 0;

	const char *fmt = argv[1];
	int argi = 2;

	for (size_t i = 0; fmt[i]; i++) {
		if (fmt[i] == '\\') {
			i++;
			if (!fmt[i])
				break;
			int c = printf_escape_char(fmt[i]);
			if (c)
				putchar(c);
			continue;
		}

		if (fmt[i] != '%') {
			putchar(fmt[i]);
			continue;
		}

		i++;

		if (!fmt[i])
			break;

		if (fmt[i] == '%') {
			putchar('%');
			continue;
		}

		const char *arg = argi < argc ? argv[argi++] : "";

		switch (fmt[i]) {
		case 's':
			fputs(arg, stdout);
			break;
		case 'c':
			putchar(arg[0] ? arg[0] : '\0');
			break;
		case 'd':
		case 'i':
			printf("%ld", strtol(arg, NULL, 0));
			break;
		case 'u':
			printf("%lu", strtoul(arg, NULL, 0));
			break;
		case 'x':
			printf("%lx", strtoul(arg, NULL, 0));
			break;
		case 'X':
			printf("%lX", strtoul(arg, NULL, 0));
			break;
		default:
			putchar('%');
			putchar(fmt[i]);
			break;
		}
	}

	return 0;
}

int sh_builtin_read(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "read: missing variable name\n");
		return 2;
	}

	if (!sh_is_name(argv[1])) {
		fprintf(stderr, "read: bad variable name: %s\n", argv[1]);
		return 2;
	}

	char *line = sh_read_line(stdin);

	if (!line)
		return 1;

	size_t n = strlen(line);

	if (n && line[n - 1] == '\n')
		line[n - 1] = '\0';

	if (setenv(argv[1], line, 1) != 0) {
		fprintf(stderr, "read: %s: %s\n", argv[1], strerror(errno));
		free(line);
		return 1;
	}

	free(line);
	return 0;
}

static int cat_stream(FILE *fp, const char *name, int show_ends,
					  int number_lines)
{
	char buf[4096];
	unsigned long line = 1;
	int at_line_start = 1;
	int last_was_newline = 1;
	int wrote_any = 0;

	for (;;) {
		size_t n = fread(buf, 1, sizeof(buf), fp);

		if (n == 0) {
			if (ferror(fp)) {
				fprintf(stderr, "cat: %s: %s\n", name, strerror(errno));
				clearerr(fp);
				return 1;
			}

			break;
		}

		for (size_t i = 0; i < n; i++) {
			unsigned char c = (unsigned char)buf[i];

			wrote_any = 1;

			if (number_lines && at_line_start) {
				printf("%6lu\t", line++);
				at_line_start = 0;
			}

			if (c == '\n') {
				if (show_ends)
					putchar('$');

				putchar('\n');
				at_line_start = 1;
				last_was_newline = 1;
			} else {
				putchar(c);
				at_line_start = 0;
				last_was_newline = 0;
			}
		}
	}

	if (wrote_any && !last_was_newline) {
		fputs("\033[7m%\033[0m\n", stdout);
	}

	return 0;
}

int sh_builtin_cat(int argc, char **argv)
{
	int show_ends = 0;
	int number_lines = 0;
	int status = 0;
	int saw_file = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			puts("usage: cat [-nE] [file...]");
			puts("  -n    number output lines");
			puts("  -E    display $ at end of each line");
			puts("  -     read from standard input");
			return 0;
		}

		if (argv[i][0] == '-' && argv[i][1]) {
			for (size_t j = 1; argv[i][j]; j++) {
				switch (argv[i][j]) {
				case 'n':
					number_lines = 1;
					break;
				case 'E':
					show_ends = 1;
					break;
				default:
					fprintf(stderr, "cat: invalid option -- '%c'\n",
							argv[i][j]);
					fprintf(stderr, "usage: cat [-nE] [file...]\n");
					return 2;
				}
			}

			continue;
		}

		saw_file = 1;

		FILE *fp;

		if (strcmp(argv[i], "-") == 0) {
			fp = stdin;
		} else {
			fp = fopen(argv[i], "rb");

			if (!fp) {
				fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
				status = 1;
				continue;
			}
		}

		int r = cat_stream(fp, argv[i], show_ends, number_lines);

		if (r)
			status = r;

		if (fp != stdin)
			fclose(fp);
	}

	if (!saw_file)
		status = cat_stream(stdin, "stdin", show_ends, number_lines);

	return status;
}
