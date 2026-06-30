#include "../../ui/console.h"

void app_editor(void)
{
    console_write("Editor avviato (stub)\n");
    while (1) {
        __asm__ volatile("hlt");
    }
}
