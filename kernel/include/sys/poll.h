#ifndef _LYR_SYS_POLL_H
#define _LYR_SYS_POLL_H

#include <stdint.h>

/* Linux/glibc-compatible poll event bits. */
#define LYR_POLLIN 0x0001
#define LYR_POLLPRI 0x0002
#define LYR_POLLOUT 0x0004
#define LYR_POLLERR 0x0008
#define LYR_POLLHUP 0x0010
#define LYR_POLLNVAL 0x0020
#define LYR_POLLRDNORM 0x0040
#define LYR_POLLRDBAND 0x0080
#define LYR_POLLWRNORM 0x0100
#define LYR_POLLWRBAND 0x0200

#define LYR_POLL_READ_MASK (LYR_POLLIN | LYR_POLLRDNORM | LYR_POLLRDBAND)
#define LYR_POLL_WRITE_MASK (LYR_POLLOUT | LYR_POLLWRNORM | LYR_POLLWRBAND)

struct lyr_pollfd {
	int fd;
	int16_t events;
	int16_t revents;
};

#endif /* _LYR_SYS_POLL_H */
