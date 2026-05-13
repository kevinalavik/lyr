#include <lib/string.h>

void *memset(void *dest, int ch, size_t count)
{
	void *ret = dest;
	__asm__ volatile("rep stosb"
					 : "+D"(dest), "+c"(count)
					 : "a"((uint8_t)ch)
					 : "memory");
	return ret;
}

void *memcpy(void *dest, const void *src, size_t count)
{
	void *ret = dest;
	__asm__ volatile("rep movsb"
					 : "+D"(dest), "+S"(src), "+c"(count)
					 :
					 : "memory");
	return ret;
}

void *memmove(void *dst, const void *src, size_t n)
{
	if (dst == src || n == 0)
		return dst;

	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	if (d < s) {
		if ((((uintptr_t)d | (uintptr_t)s) & (sizeof(uintptr_t) - 1)) == 0) {
			uintptr_t *dw = (uintptr_t *)d;
			const uintptr_t *sw = (const uintptr_t *)s;

			size_t words = n / sizeof(uintptr_t);
			size_t tail = n % sizeof(uintptr_t);

			for (size_t i = 0; i < words; i++)
				dw[i] = sw[i];

			d = (uint8_t *)(dw + words);
			s = (const uint8_t *)(sw + words);
			n = tail;
		}

		while (n--)
			*d++ = *s++;

	} else {
		d += n;
		s += n;

		if ((((uintptr_t)d | (uintptr_t)s) & (sizeof(uintptr_t) - 1)) == 0) {
			uintptr_t *dw = (uintptr_t *)d;
			const uintptr_t *sw = (const uintptr_t *)s;

			size_t words = n / sizeof(uintptr_t);
			size_t tail = n % sizeof(uintptr_t);

			for (size_t i = 0; i < words; i++)
				dw[-(ptrdiff_t)(i + 1)] = sw[-(ptrdiff_t)(i + 1)];

			d -= words * sizeof(uintptr_t);
			s -= words * sizeof(uintptr_t);
			n = tail;
		}

		while (n--)
			*--d = *--s;
	}

	return dst;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
	const uint8_t *p1 = (const uint8_t *)s1;
	const uint8_t *p2 = (const uint8_t *)s2;

	if (n == 0)
		return 0;

	while (n && (((uintptr_t)p1 | (uintptr_t)p2) & (sizeof(uintptr_t) - 1))) {
		if (*p1 != *p2)
			return *p1 - *p2;
		p1++;
		p2++;
		n--;
	}

	const uintptr_t *w1 = (const uintptr_t *)p1;
	const uintptr_t *w2 = (const uintptr_t *)p2;

	while (n >= sizeof(uintptr_t)) {
		if (*w1 != *w2) {
			const uint8_t *b1 = (const uint8_t *)w1;
			const uint8_t *b2 = (const uint8_t *)w2;

			for (size_t i = 0; i < sizeof(uintptr_t); i++) {
				if (b1[i] != b2[i])
					return b1[i] - b2[i];
			}
		}
		w1++;
		w2++;
		n -= sizeof(uintptr_t);
	}

	p1 = (const uint8_t *)w1;
	p2 = (const uint8_t *)w2;

	while (n--) {
		if (*p1 != *p2)
			return *p1 - *p2;
		p1++;
		p2++;
	}

	return 0;
}

size_t strlen(const char *str)
{
	size_t len = 0;
	while (str[len] != '\0')
		len++;
	return len;
}

char *strcpy(char *dest, const char *src)
{
	char *ret = dest;

	while ((*dest++ = *src++) != '\0')
		;

	return ret;
}

int strcmp(const char *a, const char *b)
{
	while (*a && (*a == *b)) {
		a++;
		b++;
	}

	return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	if (n == 0)
		return 0;

	while (n-- && *a && (*a == *b)) {
		if (n == 0)
			return 0;
		a++;
		b++;
	}

	return (unsigned char)*a - (unsigned char)*b;
}

char *strstr(const char *haystack, const char *needle)
{
	if (!*needle)
		return (char *)haystack;

	for (; *haystack; haystack++) {
		const char *h = haystack;
		const char *n = needle;

		while (*h && *n && *h == *n) {
			h++;
			n++;
		}

		if (!*n)
			return (char *)haystack;
	}

	return NULL;
}

char *strchr(const char *s, int c)
{
	while (*s) {
		if (*s == (char)c)
			return (char *)s;
		s++;
	}
	return NULL;
}

char *strrchr(const char *s, int c)
{
	const char *found = NULL;
	while (*s) {
		if (*s == (char)c)
			found = s;
		s++;
	}
	return (char *)found;
}

char *strcat(char *dest, const char *src)
{
	char *ret = dest;
	while (*dest)
		dest++;
	while ((*dest++ = *src++) != '\0')
		;
	return ret;
}