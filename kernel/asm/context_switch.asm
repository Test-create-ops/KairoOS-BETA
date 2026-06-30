section .text

global context_switch

context_switch:
    ; rdi = &old_rsp, rsi = new_rsp

    ; salva RSP corrente
    mov [rdi], rsp

    ; carica nuovo RSP
    mov rsp, rsi

    ; ripristina contesto dal nuovo stack (semplificato)
    ret

section .note.GNU-stack
