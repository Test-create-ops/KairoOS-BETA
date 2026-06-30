section .text

global irq1_keyboard_stub
extern irq_handler
extern keyboard_handler

irq1_keyboard_stub:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push 33
    mov rdi, [rsp]
    call irq_handler
    call keyboard_handler
    add rsp, 8
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
