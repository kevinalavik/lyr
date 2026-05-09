#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sh.h>
#include <builtin.h>

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

		putchar(' ');
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

int sh_builtin_ls(int argc, char **argv)
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

	if (!opts.long_fmt)
		putchar('\n');

	free(paths);
	return status;
}

int sh_builtin_mkdir(int argc, char **argv)
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

int sh_builtin_touch(int argc, char **argv)
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

int sh_builtin_rm(int argc, char **argv)
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

int sh_builtin_rmdir(int argc, char **argv)
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

int sh_builtin_stat(int argc, char **argv)
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