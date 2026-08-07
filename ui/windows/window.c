#include "window.h"
#include "../renderer/renderer.h"

void window_create(window_t *win, int x, int y, int w, int h, const char *title)
{
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->title = title;
    win->dragging = 0;
}

void window_draw(window_t *win)
{
    renderer_draw_window(win->x, win->y, win->w, win->h, win->title,
                         0x2D2D34, 0xDCDCE0, 0xFFFFFF);
}

void window_handle_input(window_t *win, int mx, int my, int btn)
{
    int title_h = renderer_window_title_h(win->title);

    if (btn & 1) {
        if (!win->dragging) {
            if (mx >= win->x && mx < win->x + win->w &&
                my >= win->y && my < win->y + title_h) {
                win->dragging = 1;
                win->grab_ox = mx - win->x;
                win->grab_oy = my - win->y;
            }
        }
        if (win->dragging) {
            win->x = mx - win->grab_ox;
            win->y = my - win->grab_oy;
        }
    } else {
        win->dragging = 0;
    }
}