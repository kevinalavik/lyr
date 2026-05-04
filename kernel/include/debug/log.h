#ifndef _LYR_DEBUG_LOG_H
#define _LYR_DEBUG_LOG_H

#include <dev/uart.h>
#include <util/kprintf.h>

#define _TRACE 0
#define _DEBUG 0
#define _INFO 1

#ifndef LOG_USE_COLOR
#define LOG_USE_COLOR 1
#endif

#define LOG_TRACE "trce"
#define LOG_DEBUG "dbug"
#define LOG_INFO "info"
#define LOG_WARN "warn"
#define LOG_ERR "err "

#if LOG_USE_COLOR
#define LOG_CLR_RESET "\e[0m"
#define LOG_CLR_TRACE "\e[0;97m"
#define LOG_CLR_DEBUG "\e[0;96m"
#define LOG_CLR_INFO "\e[0;37m"
#define LOG_CLR_WARN "\e[0;93m"
#define LOG_CLR_ERR "\e[0;91m"
#else
#define LOG_CLR_RESET ""
#define LOG_CLR_TRACE ""
#define LOG_CLR_DEBUG ""
#define LOG_CLR_INFO ""
#define LOG_CLR_WARN ""
#define LOG_CLR_ERR ""
#endif

#define __log(level, color, subsys, fmt, ...) \
	kprintf(color level " @ %s: " fmt LOG_CLR_RESET "\n", subsys, ##__VA_ARGS__)

#if _TRACE == 1
#define log_trace(subsys, fmt, ...) \
	__log(LOG_TRACE, LOG_CLR_TRACE, subsys, fmt, ##__VA_ARGS__)
#else
#define log_trace(subsys, fmt, ...) (void)0
#endif

#if _DEBUG == 1
#define log_debug(subsys, fmt, ...) \
	__log(LOG_DEBUG, LOG_CLR_DEBUG, subsys, fmt, ##__VA_ARGS__)
#else
#define log_debug(subsys, fmt, ...) (void)0
#endif

#if _INFO == 1
#define log_info(subsys, fmt, ...) \
	__log(LOG_INFO, LOG_CLR_INFO, subsys, fmt, ##__VA_ARGS__)
#else
#define log_info(subsys, fmt, ...) (void)0
#endif

#define log_warn(subsys, fmt, ...) \
	__log(LOG_WARN, LOG_CLR_WARN, subsys, fmt, ##__VA_ARGS__)

#define log_err(subsys, fmt, ...)                                \
	do {                                                         \
		__log(LOG_ERR, LOG_CLR_ERR, subsys, fmt, ##__VA_ARGS__); \
		uart_flush();                                            \
		kprintf_flush_lyrterm();                                 \
	} while (0)

#endif
