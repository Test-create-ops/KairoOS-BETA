#include "../../lib/framebuffer.h"
#include "../../lib/memory.h"
#include "../../drivers/input/mouse.c"

typedef struct window {
    int x, y, w, h;
    unsigned int color;
    struct window *next;
} window_t;

window_t *win_list = 0;

window_t *wm_create(int x, int y, int w, int h, unsigned int color)
{
    window_t *win = kmalloc(sizeof(window_t));
    win->x = x; win->y = y;
    win->w = w; win->h = h;
    win->color = color;
    win->next = win_list;
    win_list = win;
    return win;
}

void wm_draw(void)
{
    fb_clear(0x000000);

    window_t *w = win_list;
    while (w) {
        fb_draw_rect(w->x, w->y, w->w, w->h, w->color);
        w = w->next;
    }

    fb_draw_rect(mouse_x, mouse_y, 8, 8, 0xFFFFFF);
}
