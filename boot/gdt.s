BITS 32
global gdt_descriptor
global gdt_start

gdt_start:
    dq 0x0000000000000000      ; null
    dq 0x00CF9A000000FFFF      ; code
    dq 0x00CF92000000FFFF      ; data

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

gdt_end:
