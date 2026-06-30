#include "../../ui/console.h"

void app_fileman(void)
{
    console_write("File Manager (stub)\n");
    while (1) {
        __asm__ volatile("hlt");
    }
}
