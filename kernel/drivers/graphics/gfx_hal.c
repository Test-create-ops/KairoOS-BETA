#include <stdint.h>

void gfx_hw_putpixel(int x, int y, uint32_t color);
void gfx_hw_clear(uint32_t color);

#define FB_WIDTH  320
#define FB_HEIGHT 200
static uint32_t *fb = (uint32_t*)0xA0000;

void gfx_hw_putpixel(int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= FB_WIDTH || y >= FB_HEIGHT) return;
    fb[y * FB_WIDTH + x] = color;
}

void gfx_hw_clear(uint32_t color) {
    for (int y = 0; y < FB_HEIGHT; y++)
        for (int x = 0; x < FB_WIDTH; x++)
            fb[y * FB_WIDTH + x] = color;
}
