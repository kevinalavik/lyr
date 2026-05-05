#ifndef _LYR_SYS_SYSCALL_H
#define _LYR_SYS_SYSCALL_H

enum {
	SYS_READ = 0,
	SYS_WRITE,
	SYS_OPEN,
	SYS_CLOSE,
	SYS_STAT,
	SYS_LSEEK,
	SYS_ACCESS,
	SYS_GETDENTS,
	SYS_EXIT,
	SYS_CHMOD,
	SYS_CHOWN,
	SYS_MKDIR,
	SYS_RMDIR,
	SYS_UNLINK,
	SYS_CHROOT,
	SYS_MOUNT,
	SYS_CHANGE_ROOT,
};

#ifdef LYR_KERNEL
#include <cpu/idt.h>
void syscall_init(void);
interrupt_frame_t *syscall_dispatch(interrupt_frame_t *frame);
#endif

#endif /* _LYR_SYS_SYSCALL_H */
