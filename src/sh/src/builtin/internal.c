#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sh.h>
#include <builtin.h>

int sh_builtin_cd(int argc, char **argv)
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

int sh_builtin_pwd(void)
{
	char *cwd = sh_getcwd_alloc();
	puts(cwd);
	free(cwd);
	return 0;
}

int sh_builtin_help(void)
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
	puts("  loadkeys KEYMAP load keyboard map");
	puts("  mkdir DIR...    create directories");
	puts("  mount [-t FS] [-o DATA] [-f FLAGS] SOURCE TARGET");
	puts("  nfetch URL      fetch HTTP or raw TCP data");
	puts("  pgrep PATTERN   print PIDs whose command contains PATTERN");
	puts("  pidof NAME      print PIDs whose command exactly matches NAME");
	puts("  pinfo PID...    print /proc/PID/status");
	puts("  ping HOST       send ICMP echo requests");
	puts("  ps [-f] [PID]   list processes from /proc");
	puts("  printf FMT ...  formatted output");
	puts("  pwd             print current directory");
	puts("  read NAME       read one line into variable");
	puts("  rm [-frR] FILE... remove files or directories");
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

int sh_builtin_history(sh_shell_t *sh)
{
	for (size_t i = 0; i < sh->history_count; i++)
		printf("%4lu  %s", (unsigned long)i + 1, sh->history[i]);

	return 0;
}
