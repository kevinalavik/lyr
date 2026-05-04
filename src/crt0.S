.section .text
.global _start
.extern main

_start:
	xor %rbp, %rbp
	call main
	mov %rax, %rdi
	mov $8, %rax ; SYS_EXIT = 8
	int $0x80

.Lhalt:
	hlt
	jmp .Lhalt

.section .note.GNU-stack,"",@progbits
