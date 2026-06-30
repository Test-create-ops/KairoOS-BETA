#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

// Inizializzazione di tutti i sottosistemi del kernel
void kernel_early(void);   // GDT, IDT, TSS, PIC, APIC
void kernel_memory(uint32_t fb_addr);  // Paging, heap, mmu
void kernel_drivers(void); // Driver base (keyboard, framebuffer)
void kernel_tasks(void);   // Scheduler + processi
void kernel_main(void);    // Entry point finale

#endif
