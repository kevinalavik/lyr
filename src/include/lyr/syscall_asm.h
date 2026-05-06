#ifndef _LYR_USER_SYSCALL_ASM_H
#define _LYR_USER_SYSCALL_ASM_H

static inline long lyr_syscall3(long n, long a0, long a1, long a2)
{
	long ret;
	__asm__ volatile("syscall"
					 : "=a"(ret)
					 : "a"(n), "D"(a0), "S"(a1), "d"(a2)
					 : "rcx", "r11", "memory");
	return ret;
}

static inline long lyr_syscall5(long n, long a0, long a1, long a2, long a3,
								long a4)
{
	register long r_a3 asm("r10") = a3;
	register long r_a4 asm("r8") = a4;
	long ret;
	__asm__ volatile("syscall"
					 : "=a"(ret)
					 : "a"(n), "D"(a0), "S"(a1), "d"(a2), "r"(r_a3),
					   "r"(r_a4)
					 : "rcx", "r11", "memory");
	return ret;
}

static inline long lyr_syscall6(long n, long a0, long a1, long a2, long a3,
								long a4, long a5)
{
	register long r_a3 asm("r10") = a3;
	register long r_a4 asm("r8") = a4;
	register long r_a5 asm("r9") = a5;
	long ret;
	__asm__ volatile("syscall"
					 : "=a"(ret)
					 : "a"(n), "D"(a0), "S"(a1), "d"(a2), "r"(r_a3),
					   "r"(r_a4), "r"(r_a5)
					 : "rcx", "r11", "memory");
	return ret;
}

#endif /* _LYR_USER_SYSCALL_ASM_H */
