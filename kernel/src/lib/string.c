#include <lib/string.h>

void *memset(void *dest, int ch, size_t count)
{
	for (size_t i = 0; i < count; i++)
		((int *)dest)[i] = ch;
	return dest;
}