#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sh.h>
#include <builtin.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct proc_info {
	int pid;
	int ppid;
	unsigned threads;
	char state;
	char name[128];
	char cwd[PATH_MAX];
} proc_info_t;

static int is_pid_name(const char *s)
{
	if (!s || !*s)
		return 0;

	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (!isdigit(*p))
			return 0;
	}

	return 1;
}

static char *trim_line(char *s)
{
	while (*s && isspace((unsigned char)*s))
		s++;

	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';

	return s;
}

static int read_proc_status(int pid, proc_info_t *info)
{
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/status", pid);

	FILE *fp = fopen(path, "r");
	if (!fp)
		return -1;

	memset(info, 0, sizeof(*info));
	info->pid = pid;
	info->ppid = -1;
	info->state = '?';
	strcpy(info->name, "?");
	strcpy(info->cwd, "-");

	char line[PATH_MAX + 128];
	while (fgets(line, sizeof(line), fp)) {
		char *colon = strchr(line, ':');
		if (!colon)
			continue;

		*colon = '\0';
		char *key = line;
		char *val = trim_line(colon + 1);

		if (strcmp(key, "Name") == 0) {
			snprintf(info->name, sizeof(info->name), "%s", val);
		} else if (strcmp(key, "State") == 0) {
			info->state = *val ? *val : '?';
		} else if (strcmp(key, "Pid") == 0) {
			info->pid = atoi(val);
		} else if (strcmp(key, "PPid") == 0) {
			info->ppid = atoi(val);
		} else if (strcmp(key, "Threads") == 0) {
			info->threads = (unsigned)strtoul(val, NULL, 10);
		} else if (strcmp(key, "Cwd") == 0) {
			snprintf(info->cwd, sizeof(info->cwd), "%s", val);
		}
	}

	int err = ferror(fp) ? errno : 0;
	fclose(fp);

	if (err) {
		errno = err;
		return -1;
	}

	return 0;
}

static int print_one_ps(int pid, int full)
{
	proc_info_t info;
	if (read_proc_status(pid, &info) != 0)
		return -1;

	if (full) {
		printf("%5d %5d %c %4u %-16s %s\n", info.pid, info.ppid,
		       info.state, info.threads, info.name, info.cwd);
	} else {
		printf("%5d %5d %c %-16s\n", info.pid, info.ppid, info.state,
		       info.name);
	}

	return 0;
}

int sh_builtin_ps(int argc, char **argv)
{
	int full = 0;
	int status = 0;
	int saw_pid = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			puts("usage: ps [-f] [PID...]");
			puts("  list processes from /proc");
			puts("  -f  include thread count and cwd");
			return 0;
		}

		if (strcmp(argv[i], "-f") == 0) {
			full = 1;
			continue;
		}

		if (!is_pid_name(argv[i])) {
			fprintf(stderr, "ps: invalid PID or option: %s\n", argv[i]);
			return 2;
		}
	}

	if (full)
		printf("%5s %5s %s %4s %-16s %s\n", "PID", "PPID", "S", "THR",
		       "COMMAND", "CWD");
	else
		printf("%5s %5s %s %-16s\n", "PID", "PPID", "S", "COMMAND");

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-f") == 0)
			continue;

		saw_pid = 1;
		int pid = atoi(argv[i]);
		if (print_one_ps(pid, full) != 0) {
			fprintf(stderr, "ps: %d: %s\n", pid, strerror(errno));
			status = 1;
		}
	}

	if (saw_pid)
		return status;

	DIR *dir = opendir("/proc");
	if (!dir) {
		fprintf(stderr, "ps: /proc: %s\n", strerror(errno));
		return 1;
	}

	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (!is_pid_name(ent->d_name))
			continue;

		int pid = atoi(ent->d_name);
		if (print_one_ps(pid, full) != 0)
			status = 1;
	}

	closedir(dir);
	return status;
}

static int cat_proc_status(int pid)
{
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/status", pid);

	FILE *fp = fopen(path, "r");
	if (!fp) {
		fprintf(stderr, "pinfo: %d: %s\n", pid, strerror(errno));
		return 1;
	}

	char buf[256];
	while (fgets(buf, sizeof(buf), fp))
		fputs(buf, stdout);

	int err = ferror(fp) ? errno : 0;
	fclose(fp);

	if (err) {
		fprintf(stderr, "pinfo: %d: %s\n", pid, strerror(err));
		return 1;
	}

	return 0;
}

int sh_builtin_pinfo(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "pinfo: missing PID\n");
		return 2;
	}

	if (strcmp(argv[1], "--help") == 0) {
		puts("usage: pinfo PID...");
		puts("  print /proc/PID/status for each PID");
		return 0;
	}

	int status = 0;
	for (int i = 1; i < argc; i++) {
		if (!is_pid_name(argv[i])) {
			fprintf(stderr, "pinfo: invalid PID: %s\n", argv[i]);
			status = 2;
			continue;
		}

		if (argc > 2)
			printf("==> /proc/%s/status <==\n", argv[i]);

		if (cat_proc_status(atoi(argv[i])) != 0)
			status = 1;
	}

	return status;
}

static int process_matches(const proc_info_t *info, const char *pattern, int exact)
{
	if (exact)
		return strcmp(info->name, pattern) == 0;

	return strstr(info->name, pattern) != NULL;
}

static int find_processes(int argc, char **argv, int exact)
{
	if (argc != 2) {
		fprintf(stderr, "%s: usage: %s %s\n", argv[0], argv[0],
		        exact ? "NAME" : "PATTERN");
		return 2;
	}

	if (strcmp(argv[1], "--help") == 0) {
		printf("usage: %s %s\n", argv[0], exact ? "NAME" : "PATTERN");
		return 0;
	}

	DIR *dir = opendir("/proc");
	if (!dir) {
		fprintf(stderr, "%s: /proc: %s\n", argv[0], strerror(errno));
		return 1;
	}

	int found = 0;
	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (!is_pid_name(ent->d_name))
			continue;

		proc_info_t info;
		if (read_proc_status(atoi(ent->d_name), &info) != 0)
			continue;

		if (!process_matches(&info, argv[1], exact))
			continue;

		if (exact) {
			if (found)
				putchar(' ');
			printf("%d", info.pid);
		} else {
			printf("%d\n", info.pid);
		}

		found = 1;
	}

	closedir(dir);

	if (exact && found)
		putchar('\n');

	return found ? 0 : 1;
}

int sh_builtin_pgrep(int argc, char **argv)
{
	return find_processes(argc, argv, 0);
}

int sh_builtin_pidof(int argc, char **argv)
{
	return find_processes(argc, argv, 1);
}
