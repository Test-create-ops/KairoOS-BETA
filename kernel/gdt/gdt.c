#include "gdt.h"
#include "../tss/tss.h"

extern void gdt_flush(struct gdt_ptr*);
extern void tss_flush(uint16_t selector);

#define GDT_ENTRIES 7

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr gdtp;
struct tss64 tss;

static void gdt_set_entry(int idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt[idx].limit_low  = limit & 0xFFFF;
    gdt[idx].base_low   = base & 0xFFFF;
    gdt[idx].base_mid   = (base >> 16) & 0xFF;
    gdt[idx].access     = access;
    gdt[idx].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[idx].base_high  = (base >> 24) & 0xFF;
}

void gdt_init(void)
{
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint64_t)&gdt;

    gdt_set_entry(0, 0, 0, 0x00, 0x00);       // null
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0); // kernel code
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xA0); // kernel data
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xA0); // user code
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xA0); // user data

    uint64_t base = (uint64_t)&tss;
    uint32_t limit = sizeof(struct tss64) - 1;

    gdt[5].limit_low  = limit & 0xFFFF;
    gdt[5].base_low   = base & 0xFFFF;
    gdt[5].base_mid   = (base >> 16) & 0xFF;
    gdt[5].access     = 0x89;
    gdt[5].granularity = (limit >> 16) & 0x0F;
    gdt[5].base_high  = (base >> 24) & 0xFF;

    tss_init();
    gdt_flush(&gdtp);
    tss_flush(5 << 3);
}
