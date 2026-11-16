// host to guest: exported functions and callback trampolines, x86_64 sysv
// a frame of the arg registers goes to tsugiki_enter, which runs the guest

    .text

// frame: ints 0..47, whole vector regs 48..175, module 176, vaddr 184, stack 192, kind 200, fp result 208..223
.macro ENTRY name, kind
    .globl \name
    .type \name, @function
\name:
    push %rbp
    mov %rsp, %rbp
    sub $224, %rsp
    mov %rdi, 0(%rsp)
    mov %rsi, 8(%rsp)
    mov %rdx, 16(%rsp)
    mov %rcx, 24(%rsp)
    mov %r8, 32(%rsp)
    mov %r9, 40(%rsp)
    movdqu %xmm0, 48(%rsp)
    movdqu %xmm1, 64(%rsp)
    movdqu %xmm2, 80(%rsp)
    movdqu %xmm3, 96(%rsp)
    movdqu %xmm4, 112(%rsp)
    movdqu %xmm5, 128(%rsp)
    movdqu %xmm6, 144(%rsp)
    movdqu %xmm7, 160(%rsp)
    mov %r11, 176(%rsp)
    mov %r10, 184(%rsp)
    lea 16(%rbp), %rax
    mov %rax, 192(%rsp)
    movq $\kind, 200(%rsp)
    mov %rsp, %rdi
    call tsugiki_enter@PLT
    movdqu 208(%rsp), %xmm0
    leave
    ret
    .size \name, . - \name
.endm

// r10 = guest vaddr, r11 = module
ENTRY tsugiki_export_entry, 0
ENTRY tsugiki_jni_entry, 1
ENTRY tsugiki_jni_onload_entry, 2
// r10 = callback slot
ENTRY tsugiki_callback_entry, 3
// r10 = guest address, the host jumped there through a plain pointer and faulted
ENTRY tsugiki_raw_entry, 4

// a host function with guest args: rdi = function, rsi = HostCall
// HostCall: ints 0..47, whole vector regs 48..175, stack 176, count 184, rax 192, rdx 200, xmm0 208, xmm1 224
    .globl tsugiki_host_call
    .type tsugiki_host_call, @function
tsugiki_host_call:
    push %rbp
    mov %rsp, %rbp
    push %rbx
    push %r12
    mov %rsi, %rbx
    mov %rdi, %r11
    mov 184(%rbx), %rcx
    lea 15(,%rcx,8), %rax
    and $-16, %rax
    sub %rax, %rsp
    mov 176(%rbx), %rsi
    xor %edx, %edx
1:
    cmp %rcx, %rdx
    jae 2f
    mov (%rsi,%rdx,8), %rax
    mov %rax, (%rsp,%rdx,8)
    inc %rdx
    jmp 1b
2:
    movdqu 48(%rbx), %xmm0
    movdqu 64(%rbx), %xmm1
    movdqu 80(%rbx), %xmm2
    movdqu 96(%rbx), %xmm3
    movdqu 112(%rbx), %xmm4
    movdqu 128(%rbx), %xmm5
    movdqu 144(%rbx), %xmm6
    movdqu 160(%rbx), %xmm7
    mov 0(%rbx), %rdi
    mov 8(%rbx), %rsi
    mov 16(%rbx), %rdx
    mov 24(%rbx), %rcx
    mov 32(%rbx), %r8
    mov 40(%rbx), %r9
    // varargs callees want the vector register count in al
    mov $8, %eax
    call *%r11
    mov %rax, 192(%rbx)
    mov %rdx, 200(%rbx)
    movdqu %xmm0, 208(%rbx)
    movdqu %xmm1, 224(%rbx)
    lea -16(%rbp), %rsp
    pop %r12
    pop %rbx
    pop %rbp
    ret
    .size tsugiki_host_call, . - tsugiki_host_call

// callback trampolines, 16 bytes each: slot in r10, then the entry
    .globl tsugiki_callbacks
    .balign 16
tsugiki_callbacks:
    .set slot, 0
    .rept 8192
    .balign 16
    mov $slot, %r10d
    jmp tsugiki_callback_entry
    .set slot, slot + 1
    .endr
    .globl tsugiki_callbacks_end
tsugiki_callbacks_end:

    .section .note.GNU-stack, "", @progbits
