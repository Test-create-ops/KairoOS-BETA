#ifndef RENDERER_H
#define RENDERER_H

void renderer_draw_pixel(int x, int y, unsigned int color);
void renderer_draw_rect(int x, int y, int w, int h, unsigned int color);
void renderer_draw_text(int x, int y, const char *text, unsigned int color);

#endif
