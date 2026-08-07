; context_save.asm — kernel-mode setjmp/longjmp pair.
; Used to return from a ring3 user process (via sys_exit) back to the
; kernel GUI loop: the kernel context is saved before proc_run, and
; proc_exit restores the kernel page tables and longjmps to it.

global kernel_setjmp
global kernel_longjmp

section .text

; int kernel_setjmp(unsigned long ctx[8]);
;   ctx[0]=rip  ctx[1]=rsp  ctx[2]=rbx  ctx[3]=rbp
;   ctx[4]=r12  ctx[5]=r13  ctx[6]=r14  ctx[7]=r15
; Returns 0 on the direct call, 1 when resumed by kernel_longjmp.
kernel_setjmp:
    mov rax, [rsp]
    mov [rdi + 0], rax
    mov [rdi + 8], rsp
    mov [rdi + 16], rbx
    mov [rdi + 24], rbp
    mov [rdi + 32], r12
    mov [rdi + 40], r13
    mov [rdi + 48], r14
    mov [rdi + 56], r15
    xor eax, eax
    ret

; void kernel_longjmp(const unsigned long ctx[8]);
; Restores the saved context and returns as if kernel_setjmp returned 1.
kernel_longjmp:
    mov rax, [rdi + 0]
    mov rsp, [rdi + 8]
    mov rbx, [rdi + 16]
    mov rbp, [rdi + 24]
    mov r12, [rdi + 32]
    mov r13, [rdi + 40]
    mov r14, [rdi + 48]
    mov r15, [rdi + 56]
    push rax
    mov eax, 1
    ret

section .note.GNU-stack
