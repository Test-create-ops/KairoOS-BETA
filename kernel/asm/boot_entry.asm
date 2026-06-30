; Multiboot2 header + 32 to 64 bit transition + PVH support for QEMU -kernel
BITS 32

; PVH ELF note for QEMU -kernel direct boot
section .pvh_note
align 4
pvh_note:
    dd 4                     ; namesz = strlen("Xen")+1
    dd 8                     ; descsz = sizeof(uint64_t)
    dd 18                    ; type = XEN_ELFNOTE_PHYS32_ENTRY (0x12)
    db "Xen", 0              ; name
    dq _pvh_start            ; desc = entry physical address (64-bit)

section .multiboot
align 8
multiboot_header:
    dd 0xE85250D6
    dd 0
    dd multiboot_header_end - multiboot_header
    dd -(0xE85250D6 + 0 + (multiboot_header_end - multiboot_header))
    dw 0
    dw 0
    dd 8
multiboot_header_end:

section .bss
align 4096
pml4:
    resb 4096
pdp:
    resb 4096
pd:
    resb 4096

section .data
align 16
gdt32:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
    dq 0x00AF9A000000FFFF
    dq 0x00AF92000000FFFF
gdt32_end:

gdt64_ptr:
    dw gdt32_end - gdt32 - 1
    dq gdt32

section .text
global _start
global _pvh_start
extern kernel_main

; Helper to write char to serial
%macro serial 1
    mov dx, 0x3F8
    mov al, %1
    out dx, al
%endmacro

; PVH entry: pvh.bin jumps here in 32-bit protected mode, paging off
; EBX = pointer to hvm_start_info struct
_pvh_start:
    mov esp, 0x90000
    jmp vbe_init

_start:
    mov esp, 0x90000

    serial 'S'

    ; Check CPUID
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je no_long_mode

    serial 'C'

    ; Check long mode
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz no_long_mode

    serial 'L'

vbe_init:
    ; Scan PCI for VGA device (class 0x0300) to find the LFB BAR
    mov dword [0x70000], 0  ; default to 0 (fallback)
    xor ebx, ebx            ; device number (0-31)
.scan:
    mov eax, 0x80000000     ; enable=1, bus=0
    mov esi, ebx
    shl esi, 11             ; device number -> bits 15-11
    or eax, esi
    or eax, 0x08            ; offset 0x08 = class code register
    mov dx, 0x0CF8
    out dx, eax
    mov dx, 0x0CFC
    in eax, dx              ; read class code + revision
    shr eax, 16             ; keep upper 16 bits (class + subclass)
    cmp ax, 0x0300          ; VGA controller class?
    je .found
    inc ebx
    cmp ebx, 32             ; scan devices 0-31
    jb .scan
    jmp .done
.found:
    ; Read BAR 0 (offset 0x10)
    mov eax, 0x80000000
    mov esi, ebx
    shl esi, 11
    or eax, esi
    or eax, 0x10
    mov dx, 0x0CF8
    out dx, eax
    mov dx, 0x0CFC
    in eax, dx
    and eax, 0xFFFFFFF0
    mov [0x70000], eax
.done:

    ; Initialize Bochs VBE linear framebuffer (QEMU stdvga)
    ; Ports: index=0x01CE, data=0x01CF
    mov dx, 0x01CE
    mov ax, 4               ; VBE_DISPI_INDEX_ENABLE
    out dx, ax
    mov dx, 0x01CF
    mov ax, 0               ; disable first
    out dx, ax

    mov dx, 0x01CE
    mov ax, 1               ; VBE_DISPI_INDEX_XRES
    out dx, ax
    mov dx, 0x01CF
    mov ax, 1280
    out dx, ax
    mov word [0x70004], 1280  ; store width for kernel

    mov dx, 0x01CE
    mov ax, 2               ; VBE_DISPI_INDEX_YRES
    out dx, ax
    mov dx, 0x01CF
    mov ax, 720
    out dx, ax
    mov word [0x70008], 720   ; store height for kernel

    mov dx, 0x01CE
    mov ax, 3               ; VBE_DISPI_INDEX_BPP
    out dx, ax
    mov dx, 0x01CF
    mov ax, 32
    out dx, ax

    mov dx, 0x01CE
    mov ax, 4               ; VBE_DISPI_INDEX_ENABLE
    out dx, ax
    mov dx, 0x01CF
    mov ax, 0x41            ; ENABLED | LFB_ENABLED
    out dx, ax

init_page_tables:
    ; Identity map first 4MB with 2MB pages
    mov edi, pml4
    mov cr3, edi

    mov dword [edi], pdp + 0x003
    mov dword [edi + 4], 0

    mov edi, pdp
    mov dword [edi], pd + 0x003
    mov dword [edi + 4], 0

    mov edi, pd
    mov eax, 0x000083
    mov ecx, 2
.1:
    mov dword [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x200000
    add edi, 8
    dec ecx
    jnz .1

    serial 'P'

    ; Enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Enable long mode
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; Enable paging
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; Load 64-bit GDT
    lgdt [gdt64_ptr]

    serial 'J'

    ; Far jump to 64-bit mode
    jmp 0x18:entry64

no_long_mode:
    serial 'X'
    cli
    hlt
    jmp no_long_mode

BITS 64
entry64:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, 0x90000

    mov dx, 0x3F8
    mov al, 'O'
    out dx, al
    mov al, 'K'
    out dx, al
    mov al, 0x0D
    out dx, al
    mov al, 0x0A
    out dx, al

    call kernel_main

hang:
    cli
    hlt
    jmp hang

section .note.GNU-stack
