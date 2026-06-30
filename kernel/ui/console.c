#include "console.h"
#include "../drivers/graphics/gfx.h"

static uint32_t cw, ch;
static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;
static uint32_t char_w = 8;
static uint32_t char_h = 16;
static uint32_t fg = 0xFFFFFF;
static uint32_t bg = 0x000000;

static void draw_char(int x, int y, char c)
{
    (void)c;
    for (int yy = 0; yy < (int)char_h; yy++) {
        for (int xx = 0; xx < (int)char_w; xx++) {
            gfx_putpixel(x + xx, y + yy, fg);
        }
    }
}

void console_init(uint32_t w, uint32_t h)
{
    cw = w / char_w;
    ch = h / char_h;
    cursor_x = 0;
    cursor_y = 0;
    console_clear();
}

void console_clear(void)
{
    gfx_clear(bg);
    cursor_x = 0;
    cursor_y = 0;
}

void console_putc(char c)
{
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= ch) {
            cursor_y = 0;
            console_clear();
        }
        return;
    }

    int px = cursor_x * char_w;
    int py = cursor_y * char_h;
    draw_char(px, py, c);

    cursor_x++;
    if (cursor_x >= cw) {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= ch) {
            cursor_y = 0;
            console_clear();
        }
    }
}

void console_write(const char *s)
{
    while (*s) {
        console_putc(*s++);
    }
}
