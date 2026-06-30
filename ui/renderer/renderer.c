#include "renderer.h"

static void fb_putpixel(int x, int y, unsigned int color)
{
    (void)x; (void)y; (void)color;
}

void renderer_draw_pixel(int x, int y, unsigned int color)
{
    fb_putpixel(x, y, color);
}

void renderer_draw_rect(int x, int y, int w, int h, unsigned int color)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            fb_putpixel(xx, yy, color);
}

void renderer_draw_text(int x, int y, const char *text, unsigned int color)
{
    while (*text) {
        for (int yy = 0; yy < 8; yy++)
            for (int xx = 0; xx < 8; xx++)
                fb_putpixel(x + xx, y + yy, color);
        x += 8;
        text++;
    }
}
