#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <lyr/mount.h>

static int mount_or_report(const char *source, const char *target,
						   const char *filesystem, unsigned long flags,
						   const void *data)
{
	if (mount(source, target, filesystem, flags, data) < 0) {
		fprintf(stderr, "init: failed to mount %s at %s as %s: %s\n", source,
				target, filesystem, strerror(errno));
		return -1;
	}

	printf("init: mounted %s at %s\n", filesystem, target);
	return 0;
}

int main(void)
{
	printf("init: Welcome to lyrOS v1.0 (mlibc)\n");

	if (mount_or_report("devfs", "/dev", "devfs", 0, NULL) < 0) {
		return 1;
	}

	if (mount_or_report("tmpfs", "/tmp", "tmpfs", 0, NULL) < 0) {
		return 1;
	}

	static char path[] = "/bin/hello-world";
	static char *const argv[] = { path, NULL };
	static char *const envp[] = { NULL };

	printf("init: executing %s\n", path);
	execve(path, argv, envp);
	fprintf(stderr, "init: execve(%s) failed: %s\n", path, strerror(errno));
	return 127;
}