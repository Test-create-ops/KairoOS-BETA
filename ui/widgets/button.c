#include "button.h"
#include "../renderer/renderer.h"

void button_create(button_t *btn, int x, int y, int w, int h, const char *text) {
    btn->x = x;
    btn->y = y;
    btn->w = w;
    btn->h = h;
    btn->text = text;
}

void button_draw(button_t *btn) {
    renderer_draw_rect(btn->x, btn->y, btn->w, btn->h, 0xAAAAAA);
    renderer_draw_text(btn->x + 4, btn->y + 4, btn->text, 0x000000);
}
