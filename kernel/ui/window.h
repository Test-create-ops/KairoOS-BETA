#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

typedef struct window {
    int x, y;
    int w, h;
    uint32_t color;
    int dragging;
    int used;
    char title[32];
} window_t;

void window_manager_init(void);
window_t *window_create(const char *title, int x, int y, int w, int h, uint32_t color);
void window_manager_update(void);
void window_draw_all(void);

#endif
