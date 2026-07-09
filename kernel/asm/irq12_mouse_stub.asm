section .text

global irq12_mouse_stub
extern irq_handler
extern mouse_handler

irq12_mouse_stub:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push 44
    mov rdi, [rsp]
    call irq_handler
    call mouse_handler
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
