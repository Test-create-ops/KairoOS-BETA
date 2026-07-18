#include "../../lib/io.h"

#ifndef PCI_C
#define PCI_C

typedef struct {
    unsigned short vendor;
    unsigned short device;
    unsigned char bus;
    unsigned char slot;
    unsigned char func;
} pci_device_t;

pci_device_t pci_list[64];
int pci_count = 0;

unsigned int pci_read32(unsigned char bus, unsigned char slot, unsigned char func, unsigned char offset) {
    unsigned int address =
        (1 << 31) |
        (bus << 16) |
        (slot << 11) |
        (func << 8) |
        (offset & 0xFC);

    outl(0xCF8, address);
    return inl(0xCFC);
}

void pci_write32(unsigned char bus, unsigned char slot, unsigned char func, unsigned char offset, unsigned int value) {
    unsigned int address =
        (1 << 31) |
        (bus << 16) |
        (slot << 11) |
        (func << 8) |
        (offset & 0xFC);

    outl(0xCF8, address);
    outl(0xCFC, value);
}

void pci_scan() {
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            unsigned int data = pci_read32(bus, slot, 0, 0);
            unsigned short vendor = data & 0xFFFF;
            unsigned short device = (data >> 16) & 0xFFFF;

            if (vendor != 0xFFFF) {
                pci_list[pci_count].vendor = vendor;
                pci_list[pci_count].device = device;
                pci_list[pci_count].bus = bus;
                pci_list[pci_count].slot = slot;
                pci_list[pci_count].func = 0;
                pci_count++;
            }
        }
    }
}

#endif
