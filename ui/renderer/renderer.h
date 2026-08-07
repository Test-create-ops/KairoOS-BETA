#ifndef RENDERER_H
#define RENDERER_H

void renderer_draw_pixel(int x, int y, unsigned int color);
void renderer_draw_rect(int x, int y, int w, int h, unsigned int color);
void renderer_draw_text(int x, int y, const char *text, unsigned int color);
void renderer_draw_text_scaled(int x, int y, const char *text, unsigned int color, int scale);
void renderer_draw_window(int x, int y, int w, int h, const char *title,
                          unsigned int title_color, unsigned int bg_color,
                          unsigned int text_color);
int renderer_window_title_h(const char *title);

#endif
