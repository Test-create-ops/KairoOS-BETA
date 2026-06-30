#include "../../lib/framebuffer.h"
#include "../../lib/memory.h"
#include "../../lib/string.h"
#include "../../drivers/input/mouse.c"

typedef struct window {
    int x, y, w, h;
    char title[64];
    unsigned int color;
    struct window *next;
} window_t;

window_t *wm_list = 0;

window_t *wm_create(const char *title, int x, int y, int w, int h, unsigned int color)
{
    window_t *win = kmalloc(sizeof(window_t));
    win->x = x; win->y = y;
    win->w = w; win->h = h;
    strcpy(win->title, title);
    win->color = color;
    win->next = wm_list;
    wm_list = win;
    return win;
}

void wm_draw(void)
{
    fb_clear(0x202020);

    window_t *w = wm_list;
    while (w) {
        fb_draw_rect(w->x, w->y, w->w, w->h, w->color);
        fb_draw_rect(w->x, w->y, w->w, 20, 0x404040);
        fb_draw_text(w->x + 5, w->y + 5, w->title, 0xFFFFFF);
        w = w->next;
    }

    fb_draw_rect(mouse_x, mouse_y, 8, 8, 0xFFFFFF);
}
