#pragma once
void fb_init();
void fb_write(const char *s);
void fb_draw_rect(int x, int y, int w, int h, unsigned int color);
void fb_draw_text(int x, int y, const char *s, unsigned int color);
void fb_clear(unsigned int color);
