#ifndef LABEL_H
#define LABEL_H

typedef struct {
    int x, y;
    const char *text;
} label_t;

void label_create(label_t *lbl, int x, int y, const char *text);
void label_draw(label_t *lbl);

#endif
