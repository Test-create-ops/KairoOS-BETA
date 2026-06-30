#include "../../ui/console.h"

void app_gterm_adv(void)
{
    console_write("Terminale grafico avanzato (stub)\n");
    while (1) {
        __asm__ volatile("hlt");
    }
}
