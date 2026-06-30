#include "isr.h"
#include <stdint.h>

void isr_init(void)
{
    // qui in futuro collegherai le eccezioni (0-31) a handler C
}

void isr_handler(uint64_t vec)
{
    // handler generico per eccezioni
    // per ora: loop infinito
    (void)vec;
    while (1) {
        __asm__ volatile("hlt");
    }
}
