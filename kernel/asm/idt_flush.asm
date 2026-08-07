section .text

extern isr_handler
extern irq_handler

global idt_flush
idt_flush:
    lidt [rdi]
    ret

global isr_stub0
isr_stub0:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    ; rdi = puntatore al frame della CPU (dopo i 9 push, in fondo ci sono
    ;       i registri salvati; il frame originale [EC?][RIP][CS][RFLAGS] e' sopra)
    mov rdi, rsp
    call isr_handler
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    iretq

global irq_stub0
irq_stub0:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push 32
    mov rdi, [rsp]
    call irq_handler
    add rsp, 8
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    iretq

section .note.GNU-stack
