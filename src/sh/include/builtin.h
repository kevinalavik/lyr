#ifndef SH_BUILTIN_H
#define SH_BUILTIN_H

#include <stddef.h>
#include <sh.h>

int sh_builtin_cd(int argc, char **argv);
int sh_builtin_pwd(void);
int sh_builtin_echo(int argc, char **argv);
int sh_builtin_export(int argc, char **argv);
int sh_builtin_unset(int argc, char **argv);
int sh_builtin_env(void);
int sh_builtin_set(int argc, char **argv);
int sh_builtin_ls(int argc, char **argv);
int sh_builtin_cat(int argc, char **argv);
int sh_builtin_mkdir(int argc, char **argv);
int sh_builtin_touch(int argc, char **argv);
int sh_builtin_rm(int argc, char **argv);
int sh_builtin_rmdir(int argc, char **argv);
int sh_builtin_stat(int argc, char **argv);
int sh_builtin_type(int argc, char **argv);
int sh_builtin_which(int argc, char **argv);
int sh_builtin_read(int argc, char **argv);
int sh_builtin_printf(int argc, char **argv);
int sh_builtin_id(void);
int sh_builtin_whoami(void);
int sh_builtin_uname(int argc, char **argv);
int sh_builtin_hexdump(int argc, char **argv);
int sh_builtin_ping(int argc, char **argv);
int sh_builtin_ps(int argc, char **argv);
int sh_builtin_pinfo(int argc, char **argv);
int sh_builtin_pgrep(int argc, char **argv);
int sh_builtin_pidof(int argc, char **argv);
int sh_builtin_kill(int argc, char **argv);
int sh_builtin_nfetch(int argc, char **argv);
int sh_builtin_loadkeys(int argc, char **argv);
int sh_builtin_help(void);
int sh_builtin_history(sh_shell_t *sh);

int sh_is_builtin_name(const char *name);

#endif