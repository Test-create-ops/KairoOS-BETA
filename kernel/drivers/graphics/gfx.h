#ifndef GFX_H
#define GFX_H

#include <stdint.h>

typedef struct {
    volatile uint32_t *framebuffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
} fb_info_t;

void gfx_init(fb_info_t *info);
void gfx_putpixel(int x, int y, uint32_t color);
void gfx_clear(uint32_t color);
void gfx_rect(int x, int y, int w, int h, uint32_t color);
void gfx_fillrect(int x, int y, int w, int h, uint32_t color);
void gfx_round_rect(int x, int y, int w, int h, int r, uint32_t color);
void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint32_t color);
void gfx_print(int x, int y, uint32_t color, const char *text);
void gfx_print_scaled(int x, int y, uint32_t color, const char *text, int scale);
void gfx_drawtext(int x, int y, uint32_t color, const char *text);
int gfx_width(void);
int gfx_height(void);

#endif

