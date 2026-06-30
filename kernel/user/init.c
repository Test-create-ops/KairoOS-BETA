#include "../lib/syscall.h"

void entry(void)
{
    write("INIT: sistema operativo avviato\n");
    exec("/bin/shell");
    while (1) { __asm__ volatile("hlt"); }
}
