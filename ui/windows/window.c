#include "window.h"
#include "../renderer/renderer.h"

void window_create(window_t *win, int x, int y, int w, int h, const char *title) {
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->title = title;
}

void window_draw(window_t *win) {
    renderer_draw_rect(win->x, win->y, win->w, win->h, 0xFFFFFF);
    renderer_draw_text(win->x + 4, win->y + 4, win->title, 0x000000);
}
