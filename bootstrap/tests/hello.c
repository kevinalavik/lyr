#include <stdio.h>

int main(void)
{
	FILE *f = fopen("/dev/net/devices", "r");
	char line[256];

	printf("\033[1;36mHello, World from mlibc!\033[0m\n\n");

	if (!f) {
		printf("\033[1;31mnet:\033[0m /dev/net/devices unavailable\n");
		return 1;
	}

	printf("\033[1;34mnetwork interfaces\033[0m\n");

	while (fgets(line, sizeof(line), f)) {
		char name[32] = { 0 };
		char *p = line;

		sscanf(line, "%31s", name);

		while (*p && *p != ' ')
			p++;
		while (*p == ' ')
			p++;

		printf("  \033[1;32m%-5s\033[0m %s", name, p);
	}

	fclose(f);
	return 0;
}