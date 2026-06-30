#include "../../lib/io.h"

int mouse_x = 100;
int mouse_y = 100;

void mouse_irq(void)
{
    unsigned char status = inb(0x64);
    if (!(status & 1)) return;

    unsigned char dx = inb(0x60);
    unsigned char dy = inb(0x60);

    mouse_x += (int)(char)dx;
    mouse_y -= (int)(char)dy;
}
