; Multiboot2 + entry _start
section .multiboot
align 8
multiboot_header:
    dd 0xE85250D6
    dd 0
    dd multiboot_header_end - multiboot_header
    dd -(0xE85250D6 + 0 + (multiboot_header_end - multiboot_header))

multiboot_header_end:

section .text
global _start
extern kernel_main

_start:
    mov esp, 0x90000
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang
