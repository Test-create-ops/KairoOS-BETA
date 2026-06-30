[org 0x7C00]

global _start
_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    jmp loader_start

times 510-($-$$) db 0
dw 0xAA55
