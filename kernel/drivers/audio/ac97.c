#include "../pci/pci.c"
#include "../../lib/framebuffer.h"
#include "../../lib/io.h"

static unsigned int ac97_base = 0;

void ac97_init(void)
{
    for (int i = 0; i < pci_count; i++) {
        if (pci_list[i].vendor == 0x8086) {
            unsigned int class = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 8);
            unsigned char base = (class >> 24) & 0xFF;
            unsigned char sub  = (class >> 16) & 0xFF;

            if (base == 0x04 && sub == 0x01) {
                unsigned int bar0 = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 0x10);
                ac97_base = bar0 & ~0xF;

                fb_write("AC97 audio trovato\n");
            }
        }
    }
}

void ac97_beep(void)
{
    outw(ac97_base + 0x02, 0x0F0F);
}
