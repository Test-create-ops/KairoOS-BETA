#include "../../kernel/ui/console.h"

void app_main(void)
{
    console_write("Bluetooth Test App\n");
    while (1) {
        __asm__ volatile("hlt");
    }
}
