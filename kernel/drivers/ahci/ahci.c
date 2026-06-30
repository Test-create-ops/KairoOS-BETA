#include "../pci/pci.c"
#include "../../lib/framebuffer.h"

#define AHCI_CLASS 0x01
#define AHCI_SUBCLASS 0x06

void ahci_init(void)
{
    for (int i = 0; i < pci_count; i++) {
        if (pci_list[i].vendor != 0xFFFF) {
            unsigned int class = pci_read32(pci_list[i].bus, pci_list[i].slot, 0, 8);
            unsigned char base_class = (class >> 24) & 0xFF;
            unsigned char sub_class = (class >> 16) & 0xFF;

            if (base_class == AHCI_CLASS && sub_class == AHCI_SUBCLASS) {
                fb_write("AHCI controller trovato\n");
            }
        }
    }
}
