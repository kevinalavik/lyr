#ifndef _SYS_EPOLL_H
#define _SYS_EPOLL_H

#include <stdint.h>

#define EPOLLIN 0x001
#define EPOLLPRI 0x002
#define EPOLLOUT 0x004
#define EPOLLRDNORM 0x040
#define EPOLLWRNORM 0x100
#define EPOLLERR 0x008
#define EPOLLHUP 0x010
#define EPOLLET (1U << 31)
#define EPOLLONESHOT (1U << 30)

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLL_CLOEXEC 02000000

typedef union epoll_data {
	void *ptr;
	int fd;
	uint32_t u32;
	uint64_t u64;
} epoll_data_t;

struct epoll_event {
	uint32_t events;
	epoll_data_t data;
};

#endif /* _SYS_EPOLL_H */