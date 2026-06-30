#include "../lib/io.h"
#include "../proc/proc.h"

void apic_init(void)
{
    outl(0xFEC00000 + 0xF0, 0x100);
}

static void smp_start_cpu(int id)
{
    (void)id;
    outb(0xFEE00300, 0x00);
    outb(0xFEE00300, 0x46);
}

void smp_init(void)
{
    apic_init();
    for (int i = 1; i < 4; i++) {
        smp_start_cpu(i);
    }
}
