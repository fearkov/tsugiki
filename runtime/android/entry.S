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
