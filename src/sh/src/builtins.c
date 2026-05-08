#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sh.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1
#endif

#ifndef ICMP_ECHO
#define ICMP_ECHO 8
#endif

#ifndef ICMP_ECHOREPLY
#define ICMP_ECHOREPLY 0
#endif

#define PING_DATA_SIZE 56
#define PING_TIMEOUT_SEC 1
#define PING_RX_SPIN_LIMIT 32

static const char *const builtin_names[] = {
	"cat",	  "cd",		"clear",   "echo",	  "env",  "exit",  "export",
	"false",  "help",	"hexdump", "history", "id",	  "ls",	   "mkdir",
	"ping",	  "printf", "pwd",	   "read",	  "rm",	  "rmdir", "set",
	"source", ".",		"stat",	   "touch",	  "true", "type",  "unset",
	"which",  "whoami", NULL,
};

static int sh_is_builtin_name(const char *name)
{
	for (size_t i = 0; builtin_names[i]; i++) {
		if (strcmp(name, builtin_names[i]) == 0)
			return 1;
	}

	return 0;
}

static int bi_cd(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : getenv("HOME");
	if (!dir || !*dir)
		dir = "/";

	if (chdir(dir) != 0) {
		fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
		return 1;
	}

	return 0;
}

static int bi_pwd(void)
{
	char *cwd = sh_getcwd_alloc();
	puts(cwd);
	free(cwd);
	return 0;
}

static int bi_echo(int argc, char **argv)
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

static int bi_export(int argc, char **argv)
{
	if (argc == 1) {
		for (char **env = environ; env && *env; env++)
			printf("export %s\n", *env);
		return 0;
	}

	int status = 0;

	for (int i = 1; i < argc; i++) {
		char *eq = strchr(argv[i], '=');

		if (!eq) {
			if (!sh_is_name(argv[i])) {
				fprintf(stderr, "export: bad name: %s\n", argv[i]);
				status = 1;
			}
			continue;
		}

		char *name = sh_strndup(argv[i], (size_t)(eq - argv[i]));

		if (!sh_is_name(name)) {
			fprintf(stderr, "export: bad name: %s\n", name);
			free(name);
			status = 1;
			continue;
		}

		if (setenv(name, eq + 1, 1) != 0) {
			fprintf(stderr, "export: %s: %s\n", name, strerror(errno));
			status = 1;
		}

		free(name);
	}

	return status;
}

static int bi_unset(int argc, char **argv)
{
	int status = 0;

	for (int i = 1; i < argc; i++) {
		if (!sh_is_name(argv[i])) {
			fprintf(stderr, "unset: bad name: %s\n", argv[i]);
			status = 1;
			continue;
		}

		unsetenv(argv[i]);
	}

	return status;
}

static int bi_env(void)
{
	for (char **env = environ; env && *env; env++)
		puts(*env);

	return 0;
}

static int bi_set(int argc, char **argv)
{
	if (argc == 1)
		return bi_env();

	int status = 0;

	for (int i = 1; i < argc; i++) {
		if (!sh_is_assignment(argv[i])) {
			fprintf(stderr, "set: expected NAME=value: %s\n", argv[i]);
			status = 1;
			continue;
		}

		char *eq = strchr(argv[i], '=');
		char *name = sh_strndup(argv[i], (size_t)(eq - argv[i]));

		if (setenv(name, eq + 1, 1) != 0) {
			fprintf(stderr, "set: %s: %s\n", name, strerror(errno));
			status = 1;
		}

		free(name);
	}

	return status;
}

typedef struct ls_opts {
	int all;
	int long_fmt;
	int human;
	int recursive;
	int color;
} ls_opts_t;

typedef struct ls_entry {
	char *name;
	char *path;
	struct stat st;
	int stat_ok;
} ls_entry_t;

static int ls_name_cmp(const void *a, const void *b)
{
	const ls_entry_t *ea = a;
	const ls_entry_t *eb = b;
	return strcmp(ea->name, eb->name);
}

static char ls_type_char(mode_t mode)
{
	if (S_ISDIR(mode))
		return 'd';

#ifdef S_ISLNK
	if (S_ISLNK(mode))
		return 'l';
#endif

#ifdef S_ISCHR
	if (S_ISCHR(mode))
		return 'c';
#endif

#ifdef S_ISBLK
	if (S_ISBLK(mode))
		return 'b';
#endif

#ifdef S_ISFIFO
	if (S_ISFIFO(mode))
		return 'p';
#endif

#ifdef S_ISSOCK
	if (S_ISSOCK(mode))
		return 's';
#endif

	return '-';
}

static void ls_mode_string(mode_t mode, char out[11])
{
	out[0] = ls_type_char(mode);
	out[1] = (mode & S_IRUSR) ? 'r' : '-';
	out[2] = (mode & S_IWUSR) ? 'w' : '-';
	out[3] = (mode & S_IXUSR) ? 'x' : '-';
	out[4] = (mode & S_IRGRP) ? 'r' : '-';
	out[5] = (mode & S_IWGRP) ? 'w' : '-';
	out[6] = (mode & S_IXGRP) ? 'x' : '-';
	out[7] = (mode & S_IROTH) ? 'r' : '-';
	out[8] = (mode & S_IWOTH) ? 'w' : '-';
	out[9] = (mode & S_IXOTH) ? 'x' : '-';
	out[10] = '\0';
}

static void ls_size_string(long long size, int human, char *out, size_t out_len)
{
	static const char suffix[] = "BKMGT";

	unsigned long long n = size < 0 ? 0 : (unsigned long long)size;
	unsigned long long rem = 0;
	size_t unit = 0;

	if (!human) {
		snprintf(out, out_len, "%llu", n);
		return;
	}

	while (n >= 1024 && unit + 1 < sizeof(suffix) - 1) {
		rem = n % 1024;
		n /= 1024;
		unit++;
	}

	if (unit == 0) {
		snprintf(out, out_len, "%llu", n);
	} else if (n < 10 && rem) {
		unsigned long long tenth = (rem * 10 + 512) / 1024;

		if (tenth >= 10) {
			n++;
			tenth = 0;
		}

		snprintf(out, out_len, "%llu.%llu%c", n, tenth, suffix[unit]);
	} else {
		snprintf(out, out_len, "%llu%c", n, suffix[unit]);
	}
}

static char *ls_join_path(const char *dir, const char *name)
{
	size_t dl = strlen(dir);
	size_t nl = strlen(name);
	int slash = dl && dir[dl - 1] != '/';

	char *out = sh_xmalloc(dl + (size_t)slash + nl + 1);

	memcpy(out, dir, dl);

	if (slash)
		out[dl++] = '/';

	memcpy(out + dl, name, nl + 1);
	return out;
}

static void ls_entry_free(ls_entry_t *e)
{
	free(e->name);
	free(e->path);
}

static int ls_add_entry(ls_entry_t **entries, size_t *count, size_t *cap,
						const char *dir, const char *name)
{
	if (*count >= *cap) {
		*cap = *cap ? *cap * 2 : 32;
		*entries = sh_xrealloc(*entries, *cap * sizeof((*entries)[0]));
	}

	ls_entry_t *e = &(*entries)[(*count)++];

	memset(e, 0, sizeof(*e));

	e->name = sh_xstrdup(name);
	e->path = ls_join_path(dir, name);
	e->stat_ok = stat(e->path, &e->st) == 0;

	return 0;
}

static const char *ls_uid_name(uid_t uid, char *buf, size_t buf_len)
{
	struct passwd *pw = getpwuid(uid);

	if (pw && pw->pw_name)
		return pw->pw_name;

	snprintf(buf, buf_len, "%lu", (unsigned long)uid);
	return buf;
}

static const char *ls_gid_name(gid_t gid, char *buf, size_t buf_len)
{
	struct group *gr = getgrgid(gid);

	if (gr && gr->gr_name)
		return gr->gr_name;

	snprintf(buf, buf_len, "%lu", (unsigned long)gid);
	return buf;
}

static const char *ls_color_for_mode(mode_t mode)
{
#ifdef S_ISLNK
	if (S_ISLNK(mode))
		return "\033[1;36m";
#endif

	if (S_ISDIR(mode))
		return "\033[1;34m";

#ifdef S_ISCHR
	if (S_ISCHR(mode))
		return "\033[1;33m";
#endif

#ifdef S_ISBLK
	if (S_ISBLK(mode))
		return "\033[1;33m";
#endif

#ifdef S_ISFIFO
	if (S_ISFIFO(mode))
		return "\033[33m";
#endif

#ifdef S_ISSOCK
	if (S_ISSOCK(mode))
		return "\033[1;35m";
#endif

	if (mode & (S_IXUSR | S_IXGRP | S_IXOTH))
		return "\033[1;32m";

	return "";
}

static void ls_print_colored_name(const char *name, mode_t mode, int color)
{
	const char *c = color ? ls_color_for_mode(mode) : "";

	if (c[0])
		printf("%s%s\033[0m", c, name);
	else
		fputs(name, stdout);
}

static void ls_print_entry(const ls_opts_t *opts, const ls_entry_t *e)
{
	if (!opts->long_fmt) {
		if (e->stat_ok)
			ls_print_colored_name(e->name, e->st.st_mode, opts->color);
		else
			fputs(e->name, stdout);

		putchar('\n');
		return;
	}

	if (!e->stat_ok) {
		printf("?????????? ? ? ? ? %s\n", e->name);
		return;
	}

	char mode[11];
	char size[32];
	char uid_buf[32];
	char gid_buf[32];

	const char *user = ls_uid_name(e->st.st_uid, uid_buf, sizeof(uid_buf));
	const char *group = ls_gid_name(e->st.st_gid, gid_buf, sizeof(gid_buf));

	ls_mode_string(e->st.st_mode, mode);
	ls_size_string((long long)e->st.st_size, opts->human, size, sizeof(size));

	printf("%s %3lu %-8s %-8s %8s ", mode,
		   (unsigned long)(e->st.st_nlink ? e->st.st_nlink : 1), user, group,
		   size);

	ls_print_colored_name(e->name, e->st.st_mode, opts->color);
	putchar('\n');
}

static int ls_list_path(const char *path, const ls_opts_t *opts,
						int print_header, int *printed_any);

static int ls_list_dir(const char *path, const ls_opts_t *opts,
					   int print_header, int *printed_any)
{
	DIR *dir = opendir(path);

	if (!dir) {
		fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
		return 1;
	}

	if (print_header) {
		printf("%s:\n", path);
		*printed_any = 1;
	}

	ls_entry_t *entries = NULL;
	size_t count = 0;
	size_t cap = 0;
	struct dirent *de;

	errno = 0;

	while ((de = readdir(dir)) != NULL) {
		if (!opts->all && de->d_name[0] == '.')
			continue;

		ls_add_entry(&entries, &count, &cap, path, de->d_name);
	}

	if (errno) {
		fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
		closedir(dir);

		for (size_t i = 0; i < count; i++)
			ls_entry_free(&entries[i]);

		free(entries);
		return 1;
	}

	closedir(dir);

	qsort(entries, count, sizeof(entries[0]), ls_name_cmp);

	for (size_t i = 0; i < count; i++)
		ls_print_entry(opts, &entries[i]);

	int status = 0;

	if (opts->recursive) {
		for (size_t i = 0; i < count; i++) {
			if (!entries[i].stat_ok || !S_ISDIR(entries[i].st.st_mode))
				continue;

			if (strcmp(entries[i].name, ".") == 0 ||
				strcmp(entries[i].name, "..") == 0)
				continue;

			int r = ls_list_dir(entries[i].path, opts, 1, printed_any);

			if (r)
				status = r;
		}
	}

	for (size_t i = 0; i < count; i++)
		ls_entry_free(&entries[i]);

	free(entries);
	return status;
}

static int ls_list_file(const char *path, const ls_opts_t *opts)
{
	ls_entry_t e;
	memset(&e, 0, sizeof(e));

	const char *base = strrchr(path, '/');

	e.name = sh_xstrdup(base && base[1] ? base + 1 : path);
	e.path = sh_xstrdup(path);
	e.stat_ok = stat(path, &e.st) == 0;

	if (!e.stat_ok) {
		fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
		ls_entry_free(&e);
		return 1;
	}

	ls_print_entry(opts, &e);
	ls_entry_free(&e);
	return 0;
}

static int ls_list_path(const char *path, const ls_opts_t *opts,
						int print_header, int *printed_any)
{
	struct stat st;

	if (stat(path, &st) != 0) {
		fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
		return 1;
	}

	if (S_ISDIR(st.st_mode))
		return ls_list_dir(path, opts, print_header, printed_any);

	return ls_list_file(path, opts);
}

static int bi_ls(int argc, char **argv)
{
	ls_opts_t opts;
	memset(&opts, 0, sizeof(opts));

	opts.color = isatty(STDOUT_FILENO) ? 1 : 0;

	const char **paths =
		sh_xmalloc((size_t)(argc > 0 ? argc : 1) * sizeof(paths[0]));
	size_t path_count = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			puts("usage: ls [-alhR] [--color[=WHEN]] [file...]");
			puts("  -a                 include hidden entries");
			puts("  -l                 long listing format");
			puts("  -h                 human-readable sizes");
			puts("  -R                 list subdirectories recursively");
			puts("  --color            colorize output");
			puts("  --color=auto       colorize when stdout is a tty");
			puts("  --color=always     always colorize output");
			puts("  --color=never      never colorize output");
			free(paths);
			return 0;
		}

		if (strcmp(argv[i], "--color") == 0 ||
			strcmp(argv[i], "--color=always") == 0) {
			opts.color = 1;
			continue;
		}

		if (strcmp(argv[i], "--color=auto") == 0) {
			opts.color = isatty(STDOUT_FILENO) ? 1 : 0;
			continue;
		}

		if (strcmp(argv[i], "--color=never") == 0) {
			opts.color = 0;
			continue;
		}

		if (strncmp(argv[i], "--color=", 8) == 0) {
			fprintf(stderr, "ls: invalid color mode: %s\n", argv[i] + 8);
			free(paths);
			return 2;
		}

		if (argv[i][0] == '-' && argv[i][1]) {
			for (size_t j = 1; argv[i][j]; j++) {
				switch (argv[i][j]) {
				case 'a':
					opts.all = 1;
					break;
				case 'l':
					opts.long_fmt = 1;
					break;
				case 'h':
					opts.human = 1;
					break;
				case 'R':
					opts.recursive = 1;
					break;
				default:
					fprintf(stderr, "ls: invalid option -- '%c'\n", argv[i][j]);
					fprintf(stderr,
							"usage: ls [-alhR] [--color[=WHEN]] [file...]\n");
					free(paths);
					return 2;
				}
			}
		} else {
			paths[path_count++] = argv[i];
		}
	}

	if (path_count == 0)
		paths[path_count++] = ".";

	int status = 0;
	int printed_any = 0;
	int print_headers = path_count > 1 || opts.recursive;

	for (size_t i = 0; i < path_count; i++) {
		int r = ls_list_path(paths[i], &opts, print_headers, &printed_any);

		if (r)
			status = r;
	}

	free(paths);
	return status;
}

static int cat_stream(FILE *fp, const char *name, int show_ends,
					  int number_lines)
{
	char buf[4096];
	unsigned long line = 1;
	int at_line_start = 1;

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

			if (number_lines && at_line_start) {
				printf("%6lu\t", line++);
				at_line_start = 0;
			}

			if (c == '\n') {
				if (show_ends)
					putchar('$');
				putchar('\n');
				at_line_start = 1;
			} else {
				putchar(c);
			}
		}
	}

	return 0;
}

static int bi_cat(int argc, char **argv)
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

static int bi_mkdir(int argc, char **argv)
{
	mode_t mode = 0777;
	int status = 0;
	int saw_path = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			puts("usage: mkdir [directory...]");
			return 0;
		}

		if (argv[i][0] == '-' && argv[i][1]) {
			fprintf(stderr, "mkdir: unsupported option: %s\n", argv[i]);
			return 2;
		}

		saw_path = 1;

		if (mkdir(argv[i], mode) != 0) {
			fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(errno));
			status = 1;
		}
	}

	if (!saw_path) {
		fprintf(stderr, "mkdir: missing operand\n");
		return 2;
	}

	return status;
}

static int bi_touch(int argc, char **argv)
{
	int status = 0;
	int saw_path = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			puts("usage: touch [file...]");
			return 0;
		}

		if (argv[i][0] == '-' && argv[i][1]) {
			fprintf(stderr, "touch: unsupported option: %s\n", argv[i]);
			return 2;
		}

		saw_path = 1;

		int fd = open(argv[i], O_WRONLY | O_CREAT, 0666);
		if (fd < 0) {
			fprintf(stderr, "touch: %s: %s\n", argv[i], strerror(errno));
			status = 1;
			continue;
		}

		close(fd);
	}

	if (!saw_path) {
		fprintf(stderr, "touch: missing file operand\n");
		return 2;
	}

	return status;
}

static int bi_rm(int argc, char **argv)
{
	int force = 0;
	int status = 0;
	int saw_path = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			puts("usage: rm [-f] [file...]");
			puts("  -f    ignore missing files");
			return 0;
		}

		if (argv[i][0] == '-' && argv[i][1]) {
			for (size_t j = 1; argv[i][j]; j++) {
				switch (argv[i][j]) {
				case 'f':
					force = 1;
					break;
				default:
					fprintf(stderr, "rm: invalid option -- '%c'\n", argv[i][j]);
					fprintf(stderr, "usage: rm [-f] [file...]\n");
					return 2;
				}
			}

			continue;
		}

		saw_path = 1;

		if (unlink(argv[i]) != 0) {
			if (force && errno == ENOENT)
				continue;

			fprintf(stderr, "rm: %s: %s\n", argv[i], strerror(errno));
			status = 1;
		}
	}

	if (!saw_path && !force) {
		fprintf(stderr, "rm: missing operand\n");
		return 2;
	}

	return status;
}

static int bi_rmdir(int argc, char **argv)
{
	int status = 0;
	int saw_path = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			puts("usage: rmdir [directory...]");
			return 0;
		}

		if (argv[i][0] == '-' && argv[i][1]) {
			fprintf(stderr, "rmdir: unsupported option: %s\n", argv[i]);
			return 2;
		}

		saw_path = 1;

		if (rmdir(argv[i]) != 0) {
			fprintf(stderr, "rmdir: %s: %s\n", argv[i], strerror(errno));
			status = 1;
		}
	}

	if (!saw_path) {
		fprintf(stderr, "rmdir: missing operand\n");
		return 2;
	}

	return status;
}

static void stat_print_mode(mode_t mode)
{
	char m[11];
	ls_mode_string(mode, m);
	printf("mode:   %s (%04lo)\n", m, (unsigned long)(mode & 07777));
}

static int bi_stat(int argc, char **argv)
{
	int status = 0;

	if (argc < 2) {
		fprintf(stderr, "stat: missing operand\n");
		return 2;
	}

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			puts("usage: stat [file...]");
			return 0;
		}

		struct stat st;

		if (stat(argv[i], &st) != 0) {
			fprintf(stderr, "stat: %s: %s\n", argv[i], strerror(errno));
			status = 1;
			continue;
		}

		char uid_buf[32];
		char gid_buf[32];
		char size_buf[32];

		const char *user = ls_uid_name(st.st_uid, uid_buf, sizeof(uid_buf));
		const char *group = ls_gid_name(st.st_gid, gid_buf, sizeof(gid_buf));
		ls_size_string((long long)st.st_size, 1, size_buf, sizeof(size_buf));

		printf("  File: %s\n", argv[i]);
		printf("  Size: %lld (%s)\n", (long long)st.st_size, size_buf);
		printf(" Links: %lu\n", (unsigned long)(st.st_nlink ? st.st_nlink : 1));
		stat_print_mode(st.st_mode);
		printf("owner:  %s (%lu)\n", user, (unsigned long)st.st_uid);
		printf("group:  %s (%lu)\n", group, (unsigned long)st.st_gid);

		if (i + 1 < argc)
			putchar('\n');
	}

	return status;
}

static int bi_type(int argc, char **argv)
{
	int status = 0;

	if (argc < 2) {
		fprintf(stderr, "type: missing operand\n");
		return 2;
	}

	for (int i = 1; i < argc; i++) {
		if (sh_is_builtin_name(argv[i])) {
			printf("%s is a shell builtin\n", argv[i]);
			continue;
		}

		char *path = sh_find_in_path(argv[i]);

		if (path) {
			printf("%s is %s\n", argv[i], path);
			free(path);
			continue;
		}

		fprintf(stderr, "type: %s: not found\n", argv[i]);
		status = 1;
	}

	return status;
}

static int bi_which(int argc, char **argv)
{
	int status = 0;

	if (argc < 2) {
		fprintf(stderr, "which: missing operand\n");
		return 2;
	}

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			puts("usage: which command...");
			return 0;
		}

		char *path = sh_find_in_path(argv[i]);

		if (path) {
			puts(path);
			free(path);
			continue;
		}

		status = 1;
	}

	return status;
}

static int bi_read(int argc, char **argv)
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

static int bi_printf(int argc, char **argv)
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

static int bi_id(void)
{
	uid_t uid = getuid();
	uid_t euid = geteuid();
	gid_t gid = getgid();
	gid_t egid = getegid();

	char uid_buf[32];
	char euid_buf[32];
	char gid_buf[32];
	char egid_buf[32];

	const char *user = ls_uid_name(uid, uid_buf, sizeof(uid_buf));
	const char *euser = ls_uid_name(euid, euid_buf, sizeof(euid_buf));
	const char *group = ls_gid_name(gid, gid_buf, sizeof(gid_buf));
	const char *egroup = ls_gid_name(egid, egid_buf, sizeof(egid_buf));

	printf("uid=%lu(%s) gid=%lu(%s) euid=%lu(%s) egid=%lu(%s)\n",
		   (unsigned long)uid, user, (unsigned long)gid, group,
		   (unsigned long)euid, euser, (unsigned long)egid, egroup);

	return 0;
}

static int bi_whoami(void)
{
	uid_t uid = geteuid();
	char buf[32];
	puts(ls_uid_name(uid, buf, sizeof(buf)));
	return 0;
}

static int hexdump_file(FILE *fp, const char *name)
{
	unsigned char buf[16];
	unsigned long off = 0;

	for (;;) {
		size_t n = fread(buf, 1, sizeof(buf), fp);

		if (n == 0) {
			if (ferror(fp)) {
				fprintf(stderr, "hexdump: %s: %s\n", name, strerror(errno));
				clearerr(fp);
				return 1;
			}

			break;
		}

		printf("%08lx  ", off);

		for (size_t i = 0; i < 16; i++) {
			if (i < n)
				printf("%02x ", buf[i]);
			else
				printf("   ");

			if (i == 7)
				putchar(' ');
		}

		printf(" |");

		for (size_t i = 0; i < n; i++) {
			unsigned char c = buf[i];
			putchar((c >= 32 && c < 127) ? c : '.');
		}

		printf("|\n");

		off += (unsigned long)n;
	}

	return 0;
}

static int bi_hexdump(int argc, char **argv)
{
	int status = 0;
	int saw_file = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			puts("usage: hexdump [file...]");
			return 0;
		}

		saw_file = 1;

		FILE *fp;

		if (strcmp(argv[i], "-") == 0) {
			fp = stdin;
		} else {
			fp = fopen(argv[i], "rb");

			if (!fp) {
				fprintf(stderr, "hexdump: %s: %s\n", argv[i], strerror(errno));
				status = 1;
				continue;
			}
		}

		int r = hexdump_file(fp, argv[i]);

		if (r)
			status = r;

		if (fp != stdin)
			fclose(fp);
	}

	if (!saw_file)
		status = hexdump_file(stdin, "stdin");

	return status;
}

struct ping_packet {
	struct icmphdr hdr;
	unsigned char data[PING_DATA_SIZE];
};

static unsigned short ping_checksum(const void *buf, int len)
{
	const unsigned short *data = buf;
	unsigned int sum = 0;

	while (len > 1) {
		sum += *data++;
		len -= 2;
	}

	if (len == 1)
		sum += *(const unsigned char *)data;

	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);

	return (unsigned short)~sum;
}

static long ping_time_diff_us(const struct timeval *start,
							  const struct timeval *end)
{
	return ((end->tv_sec - start->tv_sec) * 1000000L) +
		   (end->tv_usec - start->tv_usec);
}

static int ping_parse_ipv4(const char *s, struct in_addr *out)
{
	unsigned long parts[4];
	const char *p = s;

	for (int i = 0; i < 4; i++) {
		if (!isdigit((unsigned char)*p))
			return -1;

		char *end;
		unsigned long v = strtoul(p, &end, 10);

		if (v > 255)
			return -1;

		parts[i] = v;

		if (i < 3) {
			if (*end != '.')
				return -1;
			p = end + 1;
		} else {
			if (*end)
				return -1;
		}
	}

	uint32_t addr = ((uint32_t)parts[0] << 24) | ((uint32_t)parts[1] << 16) |
					((uint32_t)parts[2] << 8) | (uint32_t)parts[3];

	out->s_addr = htonl(addr);
	return 0;
}

static int ping_send_packet(int s, const struct sockaddr_in *addr,
							unsigned short ident, int seq,
							struct timeval *start)
{
	struct ping_packet pkt;

	memset(&pkt, 0, sizeof(pkt));

	pkt.hdr.type = ICMP_ECHO;
	pkt.hdr.code = 0;
	pkt.hdr.un.echo.id = htons(ident);
	pkt.hdr.un.echo.sequence = htons((unsigned short)seq);

	for (size_t i = 0; i < sizeof(pkt.data); i++)
		pkt.data[i] = (unsigned char)i;

	pkt.hdr.checksum = 0;
	pkt.hdr.checksum = ping_checksum(&pkt, sizeof(pkt));

	gettimeofday(start, NULL);

	if (sendto(s, &pkt, sizeof(pkt), 0, (const struct sockaddr *)addr,
			   sizeof(*addr)) < 0) {
		fprintf(stderr, "ping: sendto: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int ping_parse_reply(char *buf, ssize_t n, struct icmphdr **icmp_out,
							int *ttl_out)
{
	struct icmphdr *icmp;
	int ttl = -1;

	if (n >= (ssize_t)(sizeof(struct iphdr) + sizeof(struct icmphdr))) {
		struct iphdr *ip_hdr = (struct iphdr *)buf;
		size_t ip_hdr_len = (size_t)ip_hdr->ihl * 4;

		if (ip_hdr_len < sizeof(struct iphdr))
			return -1;

		if (n < (ssize_t)(ip_hdr_len + sizeof(struct icmphdr)))
			return -1;

		ttl = ip_hdr->ttl;
		icmp = (struct icmphdr *)(buf + ip_hdr_len);
	} else if (n >= (ssize_t)sizeof(struct icmphdr)) {
		icmp = (struct icmphdr *)buf;
	} else {
		return -1;
	}

	*icmp_out = icmp;
	*ttl_out = ttl;
	return 0;
}

static int ping_recv_packet(int s, unsigned short ident, int seq,
							long *rtt_us_out, int *ttl_out,
							const struct timeval *start)
{
	char recv_buf[512];
	int spins = 0;

	while (spins++ < PING_RX_SPIN_LIMIT) {
		struct sockaddr_in from;
		socklen_t from_len = sizeof(from);
		struct timeval end;
		struct icmphdr *icmp = NULL;
		int ttl = -1;

		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(s, &rfds);

		struct timespec ts;
		ts.tv_sec = PING_TIMEOUT_SEC;
		ts.tv_nsec = 0;

		int sr = pselect(s + 1, &rfds, NULL, NULL, &ts, NULL);

		if (sr < 0) {
			fprintf(stderr, "ping: pselect failed: errno=%d %s\n", errno,
					strerror(errno));
			return -1;
		}

		if (sr == 0) {
			fprintf(stderr, "ping: pselect timed out\n");
			return -1;
		}

		if (!FD_ISSET(s, &rfds)) {
			fprintf(stderr, "ping: pselect returned %d but fd is not set\n",
					sr);
			return -1;
		}

		ssize_t n = recvfrom(s, recv_buf, sizeof(recv_buf), 0,
							 (struct sockaddr *)&from, &from_len);
		if (n < 0) {
			fprintf(stderr, "ping: recvfrom failed: errno=%d %s\n", errno,
					strerror(errno));
			return -1;
		}

		gettimeofday(&end, NULL);

		if (ping_time_diff_us(start, &end) > PING_TIMEOUT_SEC * 1000000L)
			return -1;

		if (ping_parse_reply(recv_buf, n, &icmp, &ttl) < 0)
			continue;

		if (icmp->type == ICMP_ECHO)
			continue;

		if (icmp->type == ICMP_ECHOREPLY && icmp->code == 0 &&
			ntohs(icmp->un.echo.id) == ident &&
			ntohs(icmp->un.echo.sequence) == seq) {
			if (rtt_us_out)
				*rtt_us_out = ping_time_diff_us(start, &end);

			if (ttl_out)
				*ttl_out = ttl;

			return 0;
		}
	}

	return -1;
}

static int bi_ping(int argc, char **argv)
{
	if (argc < 2 || strcmp(argv[1], "--help") == 0) {
		puts("usage: ping [-c count] ipv4-address");
		return argc < 2 ? 2 : 0;
	}

	int count = 4;
	const char *target = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "ping: -c requires a count\n");
				return 2;
			}

			count = atoi(argv[++i]);
			if (count <= 0)
				count = 1;
			continue;
		}

		if (argv[i][0] == '-' && argv[i][1]) {
			fprintf(stderr, "ping: unsupported option: %s\n", argv[i]);
			return 2;
		}

		target = argv[i];
	}

	if (!target) {
		fprintf(stderr, "ping: missing host\n");
		return 2;
	}

	struct in_addr ip;

	if (ping_parse_ipv4(target, &ip) < 0) {
		fprintf(stderr, "ping: failed to resolve %s\n", target);
		return 1;
	}

	char ipstr[INET_ADDRSTRLEN];

	if (!inet_ntop(AF_INET, &ip, ipstr, sizeof(ipstr))) {
		fprintf(stderr, "ping: inet_ntop: %s\n", strerror(errno));
		return 1;
	}

	int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);

	if (s < 0) {
		fprintf(stderr, "ping: socket: %s\n", strerror(errno));
		return 1;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr = ip;

	unsigned short ident = (unsigned short)(getpid() & 0xffff);
	int transmitted = 0;
	int received = 0;

	printf("ping: PING %s (%s): %d data bytes\n", target, ipstr,
		   PING_DATA_SIZE);

	for (int seq = 1; seq <= count; seq++) {
		struct timeval start;
		long rtt_us = 0;
		int ttl = -1;

		transmitted++;

		if (ping_send_packet(s, &addr, ident, seq, &start) < 0) {
			printf("Request failed for icmp_seq %d\n", seq);
			continue;
		}

		if (ping_recv_packet(s, ident, seq, &rtt_us, &ttl, &start) == 0) {
			received++;

			if (ttl >= 0) {
				printf(
					"%d bytes from %s: icmp_seq=%d ttl=%d time=%ld.%03ld ms\n",
					PING_DATA_SIZE + 8, ipstr, seq, ttl, rtt_us / 1000,
					rtt_us % 1000);
			} else {
				printf("%d bytes from %s: icmp_seq=%d time=%ld.%03ld ms\n",
					   PING_DATA_SIZE + 8, ipstr, seq, rtt_us / 1000,
					   rtt_us % 1000);
			}
		} else {
			printf("Request timeout for icmp_seq %d\n", seq);
		}
	}

	close(s);

	printf("--- %s ping statistics ---\n", target);
	printf("%d packets transmitted, %d packets received, %d%% packet loss\n",
		   transmitted, received,
		   transmitted == 0 ? 100 :
							  ((transmitted - received) * 100) / transmitted);

	return received > 0 ? 0 : 1;
}

static int bi_help(void)
{
	puts("builtins:");
	puts("  cat [-nE] [P]   concatenate files");
	puts("  cd [DIR]        change directory");
	puts("  clear           clear the screen");
	puts("  echo [-n] ARGS  print arguments");
	puts("  env             print environment");
	puts("  exit [STATUS]   leave shell");
	puts("  export NAME=V   set environment variable");
	puts("  false           return failure");
	puts("  hexdump [P]     dump files as hex");
	puts("  history         print command history");
	puts("  id              print user and group ids");
	puts("  ls [-alhR]      list directory contents");
	puts("  mkdir DIR...    create directories");
	puts("  ping HOST       send ICMP echo requests");
	puts("  printf FMT ...  formatted output");
	puts("  pwd             print current directory");
	puts("  read NAME       read one line into variable");
	puts("  rm [-f] FILE... remove files");
	puts("  rmdir DIR...    remove empty directories");
	puts("  set [NAME=V]    print or set variables");
	puts("  source FILE     run script in this shell");
	puts("  . FILE          same as source");
	puts("  stat FILE...    print file metadata");
	puts("  touch FILE...   create files");
	puts("  true            return success");
	puts("  type NAME...    describe command names");
	puts("  unset NAME      unset environment variable");
	puts("  which NAME...   locate external commands");
	puts("  whoami          print effective user name");
	puts("syntax:");
	puts("  # comments, ; command separator, quotes, backslash escapes");
	puts("  NAME=value assignments, $NAME and ${NAME} expansion");
	puts("prompt:");
	puts("  PS1 supports \\u, \\h, \\w, \\W, \\$, \\n");
	return 0;
}

static int bi_history(sh_shell_t *sh)
{
	for (size_t i = 0; i < sh->history_count; i++)
		printf("%4lu  %s", (unsigned long)i + 1, sh->history[i]);

	return 0;
}

int sh_builtin_run(sh_shell_t *sh, int argc, char **argv, int *handled)
{
	*handled = 1;

	if (argc == 0)
		return 0;

	if (strcmp(argv[0], "cat") == 0)
		return bi_cat(argc, argv);

	if (strcmp(argv[0], "cd") == 0)
		return bi_cd(argc, argv);

	if (strcmp(argv[0], "clear") == 0) {
		fputs("\033[2J\033[H", stdout);
		return 0;
	}

	if (strcmp(argv[0], "echo") == 0)
		return bi_echo(argc, argv);

	if (strcmp(argv[0], "env") == 0)
		return bi_env();

	if (strcmp(argv[0], "exit") == 0) {
		sh->should_exit = 1;
		return argc > 1 ? atoi(argv[1]) : sh->last_status;
	}

	if (strcmp(argv[0], "export") == 0)
		return bi_export(argc, argv);

	if (strcmp(argv[0], "false") == 0)
		return 1;

	if (strcmp(argv[0], "hexdump") == 0)
		return bi_hexdump(argc, argv);

	if (strcmp(argv[0], "history") == 0)
		return bi_history(sh);

	if (strcmp(argv[0], "id") == 0)
		return bi_id();

	if (strcmp(argv[0], "ls") == 0)
		return bi_ls(argc, argv);

	if (strcmp(argv[0], "mkdir") == 0)
		return bi_mkdir(argc, argv);

	if (strcmp(argv[0], "ping") == 0)
		return bi_ping(argc, argv);

	if (strcmp(argv[0], "printf") == 0)
		return bi_printf(argc, argv);

	if (strcmp(argv[0], "pwd") == 0)
		return bi_pwd();

	if (strcmp(argv[0], "read") == 0)
		return bi_read(argc, argv);

	if (strcmp(argv[0], "rm") == 0)
		return bi_rm(argc, argv);

	if (strcmp(argv[0], "rmdir") == 0)
		return bi_rmdir(argc, argv);

	if (strcmp(argv[0], "set") == 0)
		return bi_set(argc, argv);

	if (strcmp(argv[0], "source") == 0 || strcmp(argv[0], ".") == 0) {
		if (argc < 2) {
			fprintf(stderr, "%s: missing file\n", argv[0]);
			return 2;
		}

		return sh_run_file(sh, argv[1]);
	}

	if (strcmp(argv[0], "stat") == 0)
		return bi_stat(argc, argv);

	if (strcmp(argv[0], "touch") == 0)
		return bi_touch(argc, argv);

	if (strcmp(argv[0], "true") == 0)
		return 0;

	if (strcmp(argv[0], "type") == 0)
		return bi_type(argc, argv);

	if (strcmp(argv[0], "unset") == 0)
		return bi_unset(argc, argv);

	if (strcmp(argv[0], "which") == 0)
		return bi_which(argc, argv);

	if (strcmp(argv[0], "whoami") == 0)
		return bi_whoami();

	if (strcmp(argv[0], "help") == 0)
		return bi_help();

	*handled = 0;
	return 0;
}