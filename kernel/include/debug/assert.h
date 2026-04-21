#ifndef _LYR_DEBUG_ASSERT_H
#define _LYR_DEBUG_ASSERT_H

#include <debug/log.h>
#include <cpu/instr.h>

#define assert(cond)                                                        \
	do {                                                                    \
		if (!(cond)) {                                                      \
			log_err("kernel", "%s:%d: %s: assertion `%s` failed", __FILE__, \
					__LINE__, __func__, #cond);                             \
			nointloop();                                                    \
		}                                                                   \
	} while (0)

#define panic(fmt, ...)                                                    \
	do {                                                                   \
		log_err("kernel", "%s:%d: %s: " fmt, __FILE__, __LINE__, __func__, \
				##__VA_ARGS__);                                            \
		nointloop();                                                       \
	} while (0)

#endif // _LYR_DEBUG_ASSERT_H