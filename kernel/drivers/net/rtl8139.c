#include "../pci/pci.c"
#include "../../lib/framebuffer.h"
#include "../../lib/io.h"

static unsigned int rtl_iobase;

void rtl8139_init(void)
{
    for (int i = 0; i < pci_count; i++) {
        if (pci_list[i].vendor == 0x10EC && pci_list[i].device == 0x8139) {
            fb_write("RTL8139 trovato\n");

            unsigned int bar0 = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 0x10);
            rtl_iobase = bar0 & ~3;

            outb(rtl_iobase + 0x37, 0x10);
        }
    }
}
