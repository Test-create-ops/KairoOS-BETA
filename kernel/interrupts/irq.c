#include "irq.h"
#include "../lib/io.h"
#include <stdint.h>

static void pic_remap(void)
{
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0xFC);  // unmask IRQ0 (timer) + IRQ1 (keyboard)
    outb(0xA1, 0xEF);  // unmask IRQ12 (PS/2 mouse), mask the rest
}

void irq_init(void)
{
    pic_remap();
}

void irq_handler(uint64_t vec)
{
    if (vec >= 40)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);
}
