#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <lyr/input.h>
#include <builtin.h>

#define LOADKEYS_DEFAULT_DIR "/share/kbd/"

static const char *loadkeys_resolve_path(const char *arg, char *buf, size_t bufsz)
{
	int n;

	/*
	 * Absolute paths and explicit relative paths are used as-is.  Bare names
	 * are looked up in the standard keymap directory, so "us" becomes
	 * "/share/kbd/us".
	 */
	if (arg[0] == '/' || strchr(arg, '/'))
		return arg;

	n = snprintf(buf, bufsz, "%s%s", LOADKEYS_DEFAULT_DIR, arg);
	if (n < 0 || (size_t)n >= bufsz)
		return NULL;

	return buf;
}

int sh_builtin_loadkeys(int argc, char **argv)
{
	char path[LYR_KBD_MAP_PATH_MAX];
	const char *keymap;
	lyr_kbd_t kbd;

	if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
		puts("usage: loadkeys KEYMAP");
		puts("  Load a keyboard map.");
		puts("  Bare names are resolved below /share/kbd, e.g. 'us' => /share/kbd/us.");
		return argc < 2 ? 2 : 0;
	}

	if (argc > 2) {
		fprintf(stderr, "loadkeys: too many operands\n");
		fprintf(stderr, "usage: loadkeys KEYMAP\n");
		return 2;
	}

	keymap = loadkeys_resolve_path(argv[1], path, sizeof(path));
	if (!keymap) {
		fprintf(stderr, "loadkeys: %s: path too long\n", argv[1]);
		return 1;
	}

	if (lyr_kbd_open(&kbd) < 0) {
		fprintf(stderr, "loadkeys: %s: %s\n", LYR_KBD_DEVICE, strerror(errno));
		return 1;
	}

	if (lyr_kbd_set_layout(&kbd, keymap) < 0) {
		fprintf(stderr, "loadkeys: %s: %s\n", keymap, strerror(errno));
		lyr_kbd_close(&kbd);
		return 1;
	}

	if (lyr_kbd_close(&kbd) < 0) {
		fprintf(stderr, "loadkeys: close: %s\n", strerror(errno));
		return 1;
	}

	return 0;
}
