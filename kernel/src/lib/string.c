#include <lib/string.h>

void *memset(void *dest, int ch, size_t count)
{
	unsigned char *p = (unsigned char *)dest;
	unsigned char v = (unsigned char)ch;

	while (count--)
		*p++ = v;

	return dest;
}

void *memcpy(void *dest, const void *src, size_t count)
{
	unsigned char *d = (unsigned char *)dest;
	const unsigned char *s = (const unsigned char *)src;

	while (count--)
		*d++ = *s++;

	return dest;
}

size_t strlen(const char *str)
{
	size_t len = 0;
	while (str[len] != '\0')
		len++;
	return len;
}
