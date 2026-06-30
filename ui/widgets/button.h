#ifndef BUTTON_H
#define BUTTON_H

typedef struct {
    int x, y, w, h;
    const char *text;
} button_t;

void button_create(button_t *btn, int x, int y, int w, int h, const char *text);
void button_draw(button_t *btn);

#endif
