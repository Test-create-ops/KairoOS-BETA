#include "../../kernel/ui/console.h"

void app_main(void)
{
    console_write("Premi tasti...\n");
    while (1) {
        __asm__ volatile("hlt");
    }
}
