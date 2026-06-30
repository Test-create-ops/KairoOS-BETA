#include "../lib/framebuffer.h"
#include "../drivers/input/mouse.c"

void ui_draw_cursor(void)
{
    fb_draw_rect(mouse_x, mouse_y, 8, 8, 0xFFFFFF);
}

void ui_update(void)
{
    fb_clear(0x000000);
    ui_draw_cursor();
}
