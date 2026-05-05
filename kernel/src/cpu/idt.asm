[section .text]

%define USER_CS 0x1b
%define USER_DS 0x23
%define SYSCALL_VECTOR 0x80

%macro pushaq 0
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rsi
    push rdi
    push rbp
    push rdx
    push rcx
    push rbx
    push rax
%endmacro

%macro popaq 0
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rbp
    pop rdi
    pop rsi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
%endmacro

[global sched_iret_to_frame]
sched_iret_to_frame:
	mov ax, [rdi + 8]
	mov ds, ax
	mov ax, [rdi]
	mov es, ax
	mov rsp, rdi
	add rsp, 0x30
	popaq
	add rsp, 0x10
	iretq

[global sched_iret_to_user]
sched_iret_to_user:
	mov ax, 0x23
	mov ds, ax
	mov es, ax
	push qword 0x23
	push rsi
	push qword 0x202
	push qword 0x1b
	push rdi
	iretq

[global sched_switch_to_user]
sched_switch_to_user:
	mov cr3, rdi
	mov ax, 0x23
	mov ds, ax
	mov es, ax
	push qword 0x23
	push rdx
	push qword 0x202
	push qword 0x1b
	push rsi
	iretq

[extern isr_common_handler]
[extern syscall_dispatch]
isr_handler_stub:
	pushaq

	mov rax, cr4
	push rax
	mov rax, cr3
	push rax
	mov rax, cr2
	push rax
	mov rax, cr0
	push rax

	mov rax, ds
	push rax
	mov rax, es
	push rax

	cld
	mov rdi, rsp
	call isr_common_handler

	test rax, rax
	jnz .got_frame
	mov rax, rsp
.got_frame:
	mov rsp, rax

	add rsp, 0x30
	popaq
	add rsp, 0x10

	iretq

[global syscall_entry]
syscall_entry:
	mov [gs:8], rsp
	mov rsp, [gs:0]
	sub rsp, 0xE0

	mov qword [rsp + 0x00], USER_DS
	mov qword [rsp + 0x08], USER_DS

	mov [rsp + 0x30], rax

	mov rax, cr0
	mov [rsp + 0x10], rax
	mov rax, cr2
	mov [rsp + 0x18], rax
	mov rax, cr3
	mov [rsp + 0x20], rax
	mov rax, cr4
	mov [rsp + 0x28], rax

	mov [rsp + 0x38], rbx
	mov qword [rsp + 0x40], 0
	mov [rsp + 0x48], rdx
	mov [rsp + 0x50], rbp
	mov [rsp + 0x58], rdi
	mov [rsp + 0x60], rsi
	mov [rsp + 0x68], r8
	mov [rsp + 0x70], r9
	mov [rsp + 0x78], r10
	mov [rsp + 0x80], r11
	mov [rsp + 0x88], r12
	mov [rsp + 0x90], r13
	mov [rsp + 0x98], r14
	mov [rsp + 0xA0], r15

	mov qword [rsp + 0xA8], SYSCALL_VECTOR
	mov qword [rsp + 0xB0], 0

	mov [rsp + 0xB8], rcx
	mov qword [rsp + 0xC0], USER_CS
	mov [rsp + 0xC8], r11
	mov rax, [gs:8]
	mov [rsp + 0xD0], rax
	mov qword [rsp + 0xD8], USER_DS

	cld
	mov rdi, rsp
	call syscall_dispatch

	mov rsp, rax
	mov ax, [rsp + 8]
	mov ds, ax
	mov ax, [rsp]
	mov es, ax
	add rsp, 0x30
	popaq
	add rsp, 0x10
	iretq

%macro create_isr 1
isr_%1:
%if %1 != 8 && %1 != 10 && %1 != 11 && %1 != 12 && %1 != 13 && %1 != 14 && %1 != 17 && %1 != 30
	push 0
%endif
	push %1
	jmp isr_handler_stub
	ret
%endmacro

%assign i 0
%rep 256
create_isr i
%assign i i+1
%endrep

[section .data]

[global isr_stubs]
isr_stubs:
%assign i 0
%rep 256
	dq isr_%+i
%assign i i+1
%endrep
