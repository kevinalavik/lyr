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

size_t strlen(const char *str)
{
	size_t len = 0;
	while (str[len] != '\0')
		len++;
	return len;
}
