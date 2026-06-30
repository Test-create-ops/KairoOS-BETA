global usermode_trampoline

section .text
; usermode_trampoline(entry, user_stack)
usermode_trampoline:
    ; rdi = entry, rsi = user_stack
    mov rsp, rsi
    call rdi
.hang:
    hlt
    jmp .hang

section .note.GNU-stack
