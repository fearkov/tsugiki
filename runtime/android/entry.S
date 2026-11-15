// host to guest: exported functions and callback trampolines, x86_64 sysv
// a frame of the arg registers goes to tsugiki_enter, which runs the guest

    .text

// frame: ints 0..47, fps 48..111, module 112, vaddr 120, stack 128, kind 136, fp result 144
.macro ENTRY name, kind
    .globl \name
    .type \name, @function
\name:
    push %rbp
    mov %rsp, %rbp
    sub $160, %rsp
    mov %rdi, 0(%rsp)
    mov %rsi, 8(%rsp)
    mov %rdx, 16(%rsp)
    mov %rcx, 24(%rsp)
    mov %r8, 32(%rsp)
    mov %r9, 40(%rsp)
    movq %xmm0, 48(%rsp)
    movq %xmm1, 56(%rsp)
    movq %xmm2, 64(%rsp)
    movq %xmm3, 72(%rsp)
    movq %xmm4, 80(%rsp)
    movq %xmm5, 88(%rsp)
    movq %xmm6, 96(%rsp)
    movq %xmm7, 104(%rsp)
    mov %r11, 112(%rsp)
    mov %r10, 120(%rsp)
    lea 16(%rbp), %rax
    mov %rax, 128(%rsp)
    movq $\kind, 136(%rsp)
    mov %rsp, %rdi
    call tsugiki_enter@PLT
    movq 144(%rsp), %xmm0
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
// HostCall: ints 0..47, fps 48..111, stack 112, count 120, rax 128, rdx 136, xmm0 144, xmm1 160
    .globl tsugiki_host_call
    .type tsugiki_host_call, @function
tsugiki_host_call:
    push %rbp
    mov %rsp, %rbp
    push %rbx
    push %r12
    mov %rsi, %rbx
    mov %rdi, %r11
    mov 120(%rbx), %rcx
    lea 15(,%rcx,8), %rax
    and $-16, %rax
    sub %rax, %rsp
    mov 112(%rbx), %rsi
    xor %edx, %edx
1:
    cmp %rcx, %rdx
    jae 2f
    mov (%rsi,%rdx,8), %rax
    mov %rax, (%rsp,%rdx,8)
    inc %rdx
    jmp 1b
2:
    movq 48(%rbx), %xmm0
    movq 56(%rbx), %xmm1
    movq 64(%rbx), %xmm2
    movq 72(%rbx), %xmm3
    movq 80(%rbx), %xmm4
    movq 88(%rbx), %xmm5
    movq 96(%rbx), %xmm6
    movq 104(%rbx), %xmm7
    mov 0(%rbx), %rdi
    mov 8(%rbx), %rsi
    mov 16(%rbx), %rdx
    mov 24(%rbx), %rcx
    mov 32(%rbx), %r8
    mov 40(%rbx), %r9
    // varargs callees want the vector register count in al
    mov $8, %eax
    call *%r11
    mov %rax, 128(%rbx)
    mov %rdx, 136(%rbx)
    movdqu %xmm0, 144(%rbx)
    movdqu %xmm1, 160(%rbx)
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
