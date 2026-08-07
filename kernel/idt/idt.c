#include "idt.h"
#include "../interrupts/isr.h"
#include "../interrupts/irq.h"

extern void idt_flush(struct idt_ptr*);

#define IDT_ENTRIES 256

static struct idt_entry64 idt[IDT_ENTRIES];
static struct idt_ptr idtp;

void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t ist, uint8_t flags)
{
    idt[vector].isr_low  = handler & 0xFFFF;
    idt[vector].kernel_cs = 0x08; // kernel code segment
    idt[vector].ist      = ist;
    idt[vector].attributes = flags;
    idt[vector].isr_mid  = (handler >> 16) & 0xFFFF;
    idt[vector].isr_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].reserved = 0;
}

extern void isr_stub0(void);
extern void irq_stub0(void);
extern void irq1_keyboard_stub(void);
extern void irq12_mouse_stub(void);
extern void syscall_stub(void);

void idt_init(void)
{
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint64_t)&idt;

    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt[i].isr_low = 0;
        idt[i].kernel_cs = 0;
        idt[i].ist = 0;
        idt[i].attributes = 0;
        idt[i].isr_mid = 0;
        idt[i].isr_high = 0;
        idt[i].reserved = 0;
    }

    idt_set_gate(0, (uint64_t)isr_stub0, 0, 0x8E);
    idt_set_gate(32, (uint64_t)irq_stub0, 0, 0x8E);
    idt_set_gate(33, (uint64_t)irq1_keyboard_stub, 0, 0x8E);
    idt_set_gate(44, (uint64_t)irq12_mouse_stub, 0, 0x8E);
    idt_set_gate(0x80, (uint64_t)syscall_stub, 0, 0xEE);

    isr_init();
    irq_init();

    idt_flush(&idtp);
}
