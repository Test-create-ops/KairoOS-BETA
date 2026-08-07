#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

typedef struct {
    int x, y;
    int w, h;
    const char *title;
    int dragging;
    int grab_ox, grab_oy;
} window_t;

void window_create(window_t *win, int x, int y, int w, int h, const char *title);
void window_draw(window_t *win);
void window_handle_input(window_t *win, int mx, int my, int btn);

#endif