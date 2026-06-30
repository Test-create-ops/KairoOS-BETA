#include "label.h"
#include "../renderer/renderer.h"

void label_create(label_t *lbl, int x, int y, const char *text) {
    lbl->x = x;
    lbl->y = y;
    lbl->text = text;
}

void label_draw(label_t *lbl) {
    renderer_draw_text(lbl->x, lbl->y, lbl->text, 0xFFFFFF);
}
