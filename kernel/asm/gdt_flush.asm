section .text

global gdt_flush
gdt_flush:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    push 0x08
    lea rax, [rel .next]
    push rax
    retfq
.next:
    ret

section .note.GNU-stack
