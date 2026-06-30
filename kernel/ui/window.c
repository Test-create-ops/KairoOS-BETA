#include "window.h"
#include "../drivers/graphics/gfx.h"
#include "../drivers/mouse/mouse.h"

#define MAX_WINDOWS 32
static window_t windows[MAX_WINDOWS];
static window_t *focused = 0;

static void draw_window(window_t *w)
{
    // bordo
    for (int xx = 0; xx < w->w; xx++) {
        gfx_putpixel(w->x + xx, w->y, 0xFFFFFF);
        gfx_putpixel(w->x + xx, w->y + w->h - 1, 0xFFFFFF);
    }
    for (int yy = 0; yy < w->h; yy++) {
        gfx_putpixel(w->x, w->y + yy, 0xFFFFFF);
        gfx_putpixel(w->x + w->w - 1, w->y + yy, 0xFFFFFF);
    }

    // titolo
    gfx_fillrect(w->x + 1, w->y + 1, w->w - 2, 20, 0x3333FF);
    gfx_drawtext(w->x + 5, w->y + 5, 0xFFFFFF, w->title);

    // contenuto
    gfx_fillrect(w->x + 1, w->y + 21, w->w - 2, w->h - 22, w->color);
}

void window_manager_init(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        windows[i].used = 0;
}

window_t *window_create(const char *title, int x, int y, int w, int h, uint32_t color)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].used) {
            windows[i].used = 1;
            windows[i].x = x;
            windows[i].y = y;
            windows[i].w = w;
            windows[i].h = h;
            windows[i].color = color;
            windows[i].dragging = 0;

            int k = 0;
            while (title[k] && k < 31) {
                windows[i].title[k] = title[k];
                k++;
            }
            windows[i].title[k] = 0;

            return &windows[i];
        }
    }
    return 0;
}

void window_manager_update(void)
{
    int mx = mouse_get_x();
    int my = mouse_get_y();
    uint8_t mb = mouse_get_buttons();

    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        if (!windows[i].used) continue;

        window_t *w = &windows[i];

        // click sul titolo
        if (mb & 1) {
            if (mx >= w->x && mx <= w->x + w->w &&
                my >= w->y && my <= w->y + 20) {
                focused = w;
                w->dragging = 1;
            }
        } else {
            w->dragging = 0;
        }

        if (w->dragging) {
            w->x = mx - w->w / 2;
            w->y = my - 10;
        }
    }
}

void window_draw_all(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].used)
            draw_window(&windows[i]);
    }

    // cursore del mouse
    int mx = mouse_get_x();
    int my = mouse_get_y();
    gfx_fillrect(mx, my, 8, 8, 0xFF0000);
}
