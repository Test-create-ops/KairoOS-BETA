section .text

global tss_flush
tss_flush:
    mov ax, di
    ltr ax
    ret

section .note.GNU-stack
