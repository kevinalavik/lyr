#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ps_info {
	int pid;
	int ppid;
	int threads;
	char state;
	char mode;
	char supervised;
	char name[64];
	char cwd[256];
} ps_info_t;

static int ps_is_number(const char *s)
{
	if (!s || !*s)
		return 0;

	for (; *s; s++) {
		if (!isdigit((unsigned char)*s))
			return 0;
	}

	return 1;
}

static void ps_chomp(char *s)
{
	size_t n;

	if (!s)
		return;

	n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
		s[--n] = '\0';
}

static const char *ps_value_after_colon(char *line)
{
	char *p = strchr(line, ':');

	if (!p)
		return NULL;

	p++;

	while (*p == ' ' || *p == '\t')
		p++;

	return p;
}

static void ps_info_init(ps_info_t *info, int pid)
{
	memset(info, 0, sizeof(*info));

	info->pid = pid;
	info->ppid = -1;
	info->threads = -1;
	info->state = '?';
	info->mode = '?';
	info->supervised = '?';

	strcpy(info->name, "?");
	strcpy(info->cwd, "-");
}

static int ps_read_status(int pid, ps_info_t *info)
{
	char path[64];
	char line[512];
	FILE *fp;

	ps_info_init(info, pid);

	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	fp = fopen(path, "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		ps_chomp(line);

		if (strncmp(line, "Name:", 5) == 0) {
			const char *v = ps_value_after_colon(line);
			if (v && *v) {
				strncpy(info->name, v, sizeof(info->name) - 1);
				info->name[sizeof(info->name) - 1] = '\0';
			}
		} else if (strncmp(line, "State:", 6) == 0) {
			const char *v = ps_value_after_colon(line);
			if (v && *v)
				info->state = *v;
		} else if (strncmp(line, "Mode:", 5) == 0) {
			const char *v = ps_value_after_colon(line);
			if (v && *v)
				info->mode = *v;
		} else if (strncmp(line, "Supervised:", 11) == 0) {
			const char *v = ps_value_after_colon(line);
			if (v && *v)
				info->supervised = *v;
		} else if (strncmp(line, "Pid:", 4) == 0) {
			const char *v = ps_value_after_colon(line);
			if (v)
				info->pid = atoi(v);
		} else if (strncmp(line, "PPid:", 5) == 0) {
			const char *v = ps_value_after_colon(line);
			if (v)
				info->ppid = atoi(v);
		} else if (strncmp(line, "Threads:", 8) == 0) {
			const char *v = ps_value_after_colon(line);
			if (v)
				info->threads = atoi(v);
		} else if (strncmp(line, "Cwd:", 4) == 0) {
			const char *v = ps_value_after_colon(line);
			if (v && *v) {
				strncpy(info->cwd, v, sizeof(info->cwd) - 1);
				info->cwd[sizeof(info->cwd) - 1] = '\0';
			}
		}
	}

	fclose(fp);
	return 0;
}

static int ps_should_show(const ps_info_t *info, int show_all)
{
	if (show_all)
		return 1;

	if (info->mode == 'K')
		return 0;

	return 1;
}

static void ps_print_header(int full)
{
	if (full) {
		printf("%5s %5s %-1s %-1s %-3s %4s %-16s %s\n", "PID", "PPID", "S", "M",
			   "SUP", "THR", "COMMAND", "CWD");
	} else {
		printf("%5s %5s %-1s %-1s %-3s %-16s\n", "PID", "PPID", "S", "M", "SUP",
			   "COMMAND");
	}
}

static void ps_print_one(const ps_info_t *info, int full)
{
	if (full) {
		printf("%5d %5d %-1c %-1c %-3c %4d %-16.16s %s\n", info->pid,
			   info->ppid, info->state, info->mode, info->supervised,
			   info->threads, info->name, info->cwd);
	} else {
		printf("%5d %5d %-1c %-1c %-3c %-16.16s\n", info->pid, info->ppid,
			   info->state, info->mode, info->supervised, info->name);
	}
}

static int ps_cmp_pid(const void *a, const void *b)
{
	const ps_info_t *pa = a;
	const ps_info_t *pb = b;

	if (pa->pid < pb->pid)
		return -1;
	if (pa->pid > pb->pid)
		return 1;
	return 0;
}

int sh_builtin_ps(int argc, char **argv)
{
	int full = 0;
	int show_all = 0;
	int status = 0;
	int saw_pid = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			puts("usage: ps [-afh] [pid...]");
			puts("");
			puts("List processes from /proc.");
			puts("");
			puts("Options:");
			puts("  -a    show all processes, including kernel tasks");
			puts("  -f    full output, including thread count and cwd");
			puts("  -h    show this help");
			puts("");
			puts("Columns:");
			puts("  PID      process id");
			puts("  PPID     parent process id; 0 means no userspace parent");
			puts("  S        process state, e.g. R = running/runnable");
			puts("  M        process mode: U = userspace, K = kernel task");
			puts(
				"  SUP      supervision: S = supervised by userspace parent, - = unsupervised");
			puts("  THR      thread count; only shown with -f");
			puts("  COMMAND  process command/name");
			puts("  CWD      current working directory; only shown with -f");
			puts("");
			puts("Examples:");
			puts("  ps       show user-facing processes");
			puts("  ps -a    show userspace and kernel processes");
			puts("  ps -f    show full details");
			puts("  ps 1 7   show selected PIDs");
			return 0;
		}

		if (argv[i][0] == '-' && argv[i][1]) {
			for (size_t j = 1; argv[i][j]; j++) {
				switch (argv[i][j]) {
				case 'a':
					show_all = 1;
					break;
				case 'f':
					full = 1;
					break;
				default:
					fprintf(stderr, "ps: invalid option -- '%c'\n", argv[i][j]);
					fprintf(stderr, "usage: ps [-a] [-f] [pid...]\n");
					return 2;
				}
			}
			continue;
		}

		if (!ps_is_number(argv[i])) {
			fprintf(stderr, "ps: invalid pid: %s\n", argv[i]);
			return 2;
		}

		saw_pid = 1;
	}

	ps_print_header(full);

	if (saw_pid) {
		for (int i = 1; i < argc; i++) {
			if (argv[i][0] == '-' && argv[i][1])
				continue;

			ps_info_t info;
			int pid;

			if (!ps_is_number(argv[i]))
				continue;

			pid = atoi(argv[i]);

			if (ps_read_status(pid, &info) < 0) {
				fprintf(stderr, "ps: %d: %s\n", pid, strerror(errno));
				status = 1;
				continue;
			}

			if (ps_should_show(&info, show_all))
				ps_print_one(&info, full);
		}

		return status;
	}

	DIR *dir = opendir("/proc");
	if (!dir) {
		fprintf(stderr, "ps: /proc: %s\n", strerror(errno));
		return 1;
	}

	ps_info_t *list = NULL;
	size_t count = 0;
	size_t cap = 0;

	for (;;) {
		struct dirent *de = readdir(dir);
		if (!de)
			break;

		if (!ps_is_number(de->d_name))
			continue;

		int pid = atoi(de->d_name);
		ps_info_t info;

		if (ps_read_status(pid, &info) < 0)
			continue;

		if (!ps_should_show(&info, show_all))
			continue;

		if (count == cap) {
			size_t new_cap = cap ? cap * 2 : 16;
			ps_info_t *new_list = realloc(list, new_cap * sizeof(*new_list));

			if (!new_list) {
				fprintf(stderr, "ps: out of memory\n");
				free(list);
				closedir(dir);
				return 1;
			}

			list = new_list;
			cap = new_cap;
		}

		list[count++] = info;
	}

	closedir(dir);

	qsort(list, count, sizeof(*list), ps_cmp_pid);

	for (size_t i = 0; i < count; i++)
		ps_print_one(&list[i], full);

	free(list);
	return status;
}

int sh_builtin_pinfo(int argc, char **argv)
{
	int status = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: pinfo pid...\n");
		return 2;
	}

	for (int i = 1; i < argc; i++) {
		char path[64];
		char buf[4096];
		FILE *fp;

		if (!ps_is_number(argv[i])) {
			fprintf(stderr, "pinfo: invalid pid: %s\n", argv[i]);
			status = 2;
			continue;
		}

		snprintf(path, sizeof(path), "/proc/%s/status", argv[i]);

		fp = fopen(path, "r");
		if (!fp) {
			fprintf(stderr, "pinfo: %s: %s\n", argv[i], strerror(errno));
			status = 1;
			continue;
		}

		while (fgets(buf, sizeof(buf), fp))
			fputs(buf, stdout);

		fclose(fp);
	}

	return status;
}

int sh_builtin_pgrep(int argc, char **argv)
{
	const char *pattern;
	DIR *dir;
	int found = 0;

	if (argc != 2) {
		fprintf(stderr, "usage: pgrep pattern\n");
		return 2;
	}

	pattern = argv[1];

	dir = opendir("/proc");
	if (!dir) {
		fprintf(stderr, "pgrep: /proc: %s\n", strerror(errno));
		return 1;
	}

	for (;;) {
		struct dirent *de = readdir(dir);
		if (!de)
			break;

		if (!ps_is_number(de->d_name))
			continue;

		ps_info_t info;
		int pid = atoi(de->d_name);

		if (ps_read_status(pid, &info) < 0)
			continue;

		if (info.mode == 'K')
			continue;

		if (strstr(info.name, pattern)) {
			printf("%d\n", info.pid);
			found = 1;
		}
	}

	closedir(dir);
	return found ? 0 : 1;
}

int sh_builtin_pidof(int argc, char **argv)
{
	const char *name;
	DIR *dir;
	int found = 0;
	int first = 1;

	if (argc != 2) {
		fprintf(stderr, "usage: pidof name\n");
		return 2;
	}

	name = argv[1];

	dir = opendir("/proc");
	if (!dir) {
		fprintf(stderr, "pidof: /proc: %s\n", strerror(errno));
		return 1;
	}

	for (;;) {
		struct dirent *de = readdir(dir);
		if (!de)
			break;

		if (!ps_is_number(de->d_name))
			continue;

		ps_info_t info;
		int pid = atoi(de->d_name);

		if (ps_read_status(pid, &info) < 0)
			continue;

		if (info.mode == 'K')
			continue;

		if (strcmp(info.name, name) == 0) {
			if (!first)
				putchar(' ');

			printf("%d", info.pid);
			first = 0;
			found = 1;
		}
	}

	closedir(dir);

	if (found)
		putchar('\n');

	return found ? 0 : 1;
}