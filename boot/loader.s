BITS 16
extern switch_to_pm

global loader_start
loader_start:
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:switch_to_pm
