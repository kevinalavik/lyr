#ifndef _LYR_DEBUG_LOG_H
#define _LYR_DEBUG_LOG_H

#include <util/kprintf.h>

#define ok(fmt, ...) kprintf("\e[0;32m[  OK  ]\e[0m " fmt "\n", ##__VA_ARGS__)
#define warn(fmt, ...) kprintf("\e[0;33m[ WARN ]\e[0m " fmt "\n", ##__VA_ARGS__)
#define error(fmt, ...) \
	kprintf("\e[0;31m[ FAIL ]\e[0m " fmt "\n", ##__VA_ARGS__)

#endif // _LYR_DEBUG_LOG_H