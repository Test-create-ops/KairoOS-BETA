#include "framebuffer.h"

static struct {
    unsigned int *fb;
    int w, h;
} fb_state;

void fb_init(void)
{
    fb_state.fb = (unsigned int *)0xFD000000;
    fb_state.w = 1280;
    fb_state.h = 720;
}

void fb_write(const char *s)
{
    (void)s;
}

void fb_draw_rect(int x, int y, int w, int h, unsigned int color)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if (xx >= 0 && yy >= 0 && xx < fb_state.w && yy < fb_state.h)
                fb_state.fb[yy * fb_state.w + xx] = color;
}

void fb_draw_text(int x, int y, const char *s, unsigned int color)
{
    (void)x; (void)y; (void)s; (void)color;
}

void fb_clear(unsigned int color)
{
    for (int i = 0; i < fb_state.w * fb_state.h; i++)
        fb_state.fb[i] = color;
}
