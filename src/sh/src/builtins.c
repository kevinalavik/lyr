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
#include <netdb.h>
#include <sh.h>
#include <builtin.h>

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

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

#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif

#ifndef SO_RCVTIMEO
#define SO_RCVTIMEO 20
#endif

#define PING_DATA_SIZE 56
#define PING_TIMEOUT_SEC 1
#define PING_RX_SPIN_LIMIT 32

int sh_builtin_run(sh_shell_t *sh, int argc, char **argv, int *handled)
{
	*handled = 1;

	if (argc == 0)
		return 0;

	if (strcmp(argv[0], "cat") == 0)
		return sh_builtin_cat(argc, argv);

	if (strcmp(argv[0], "cd") == 0)
		return sh_builtin_cd(argc, argv);

	if (strcmp(argv[0], "clear") == 0) {
		fputs("\033[2J\033[H", stdout);
		return 0;
	}

	if (strcmp(argv[0], "echo") == 0)
		return sh_builtin_echo(argc, argv);

	if (strcmp(argv[0], "env") == 0)
		return sh_builtin_env();

	if (strcmp(argv[0], "exit") == 0) {
		sh->should_exit = 1;
		return argc > 1 ? atoi(argv[1]) : sh->last_status;
	}

	if (strcmp(argv[0], "export") == 0)
		return sh_builtin_export(argc, argv);

	if (strcmp(argv[0], "false") == 0)
		return 1;

	if (strcmp(argv[0], "hexdump") == 0)
		return sh_builtin_hexdump(argc, argv);

	if (strcmp(argv[0], "history") == 0)
		return sh_builtin_history(sh);

	if (strcmp(argv[0], "id") == 0)
		return sh_builtin_id();

	if (strcmp(argv[0], "ls") == 0)
		return sh_builtin_ls(argc, argv);

	if (strcmp(argv[0], "mkdir") == 0)
		return sh_builtin_mkdir(argc, argv);

	if (strcmp(argv[0], "nfetch") == 0)
		return sh_builtin_nfetch(argc, argv);

	if (strcmp(argv[0], "pgrep") == 0)
		return sh_builtin_pgrep(argc, argv);

	if (strcmp(argv[0], "pidof") == 0)
		return sh_builtin_pidof(argc, argv);

	if (strcmp(argv[0], "pinfo") == 0)
		return sh_builtin_pinfo(argc, argv);

	if (strcmp(argv[0], "ping") == 0)
		return sh_builtin_ping(argc, argv);

	if (strcmp(argv[0], "ps") == 0)
		return sh_builtin_ps(argc, argv);

	if (strcmp(argv[0], "printf") == 0)
		return sh_builtin_printf(argc, argv);

	if (strcmp(argv[0], "pwd") == 0)
		return sh_builtin_pwd();

	if (strcmp(argv[0], "read") == 0)
		return sh_builtin_read(argc, argv);

	if (strcmp(argv[0], "rm") == 0)
		return sh_builtin_rm(argc, argv);

	if (strcmp(argv[0], "rmdir") == 0)
		return sh_builtin_rmdir(argc, argv);

	if (strcmp(argv[0], "set") == 0)
		return sh_builtin_set(argc, argv);

	if (strcmp(argv[0], "source") == 0 || strcmp(argv[0], ".") == 0) {
		if (argc < 2) {
			fprintf(stderr, "%s: missing file\n", argv[0]);
			return 2;
		}

		return sh_run_file(sh, argv[1]);
	}

	if (strcmp(argv[0], "stat") == 0)
		return sh_builtin_stat(argc, argv);

	if (strcmp(argv[0], "touch") == 0)
		return sh_builtin_touch(argc, argv);

	if (strcmp(argv[0], "true") == 0)
		return 0;

	if (strcmp(argv[0], "type") == 0)
		return sh_builtin_type(argc, argv);

	if (strcmp(argv[0], "unset") == 0)
		return sh_builtin_unset(argc, argv);

	if (strcmp(argv[0], "which") == 0)
		return sh_builtin_which(argc, argv);

	if (strcmp(argv[0], "whoami") == 0)
		return sh_builtin_whoami();

	if (strcmp(argv[0], "help") == 0)
		return sh_builtin_help();

	*handled = 0;
	return 0;
}

int sh_builtin_nfetch(int argc, char **argv)
{
	extern int nfetch_run(int argc, char **argv);
	return nfetch_run(argc, argv);
}