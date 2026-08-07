global syscall_stub

section .text

extern syscall_handler

syscall_stub:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11

    mov rdi, [rsp + 64]     ; syscall number (saved rax)
    mov rsi, rbx            ; arg1
    mov rdx, [rsp + 56]     ; arg2 (saved rcx)
    mov rcx, [rsp + 48]     ; arg3 (saved rdx)
    call syscall_handler
    mov [rsp + 64], rax

    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    iretq

section .note.GNU-stack
