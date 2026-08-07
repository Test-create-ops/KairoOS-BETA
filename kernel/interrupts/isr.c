#include "isr.h"
#include <stdint.h>

extern void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t ist, uint8_t flags);
extern void isr_stub0(void);

void isr_init(void)
{
    for (int i = 1; i < 32; i++)
        idt_set_gate(i, (uint64_t)isr_stub0, 0, 0x8E);
}

static void dbg_putc(char c)
{
    __asm__ volatile("outb %0, %1" :: "a"((uint8_t)c), "Nd"((uint16_t)0x3F8));
}

static void dbg_puts(const char *s)
{
    while (*s) dbg_putc(*s++);
}

static void dbg_hex(uint64_t v)
{
    char buf[17];
    const char *h = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) {
        buf[i] = h[(v >> (60 - i * 4)) & 0xF];
    }
    buf[16] = 0;
    dbg_puts(buf);
}

// frame: [rax rcx rdx rsi rdi r8 r9 r10 r11] poi il frame CPU
//   senza error code: [RIP CS RFLAGS]
//   con error code:   [EC RIP CS RFLAGS]
void isr_handler(uint64_t *frame)
{
    uint64_t rip = 0;
    if (frame[10] == 0x08)
        rip = frame[9];
    else if (frame[11] == 0x08)
        rip = frame[10];
    dbg_puts("\r\nISR RIP=");
    dbg_hex(rip);
    dbg_puts(" RBP=");
    dbg_hex((uint64_t)frame - 72);
    dbg_puts(" FRM9=");
    dbg_hex(frame[9]);
    dbg_puts(" FRM10=");
    dbg_hex(frame[10]);
    dbg_puts(" FRM11=");
    dbg_hex(frame[11]);
    dbg_puts("\r\n");
    while (1) {
        __asm__ volatile("hlt");
    }
}
