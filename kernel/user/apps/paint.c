#include "../../ui/console.h"

void app_paint(void)
{
    console_write("Paint (stub)\n");
    while (1) {
        __asm__ volatile("hlt");
    }
}
