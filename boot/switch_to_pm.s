BITS 32
extern kernel_main
global switch_to_pm

switch_to_pm:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x90000

    call kernel_main

.hang:
    jmp .hang
