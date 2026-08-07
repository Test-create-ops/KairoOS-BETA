global usermode_trampoline

section .text
; usermode_trampoline(entry, user_stack)
; Passaggio reale a ring3 via iretq.
usermode_trampoline:
    ; rdi = entry, rsi = user_stack
    mov ax, 0x23            ; user data | RPL3
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rax, 0x23
    push rax                ; SS
    push rsi                ; user RSP
    mov rax, 0x202          ; RFLAGS: IF=1
    push rax
    mov rax, 0x1B           ; CS = user code | RPL3
    push rax
    push rdi                ; RIP
    iretq

section .note.GNU-stack
