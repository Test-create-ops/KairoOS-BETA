#include "../../ui/console.h"

void app_gterm(void)
{
    console_write("Terminale grafico (stub)\n");
    while (1) {
        __asm__ volatile("hlt");
    }
}
