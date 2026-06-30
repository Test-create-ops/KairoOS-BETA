#include "mouse.h"
#include "../graphics/gfx.h"

#define PS2_DATA 0x60
#define PS2_STATUS 0x64

static int mouse_x = 100;
static int mouse_y = 100;
static uint8_t mouse_buttons = 0;

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void mouse_init(void)
{
    mouse_x = gfx_width() / 2;
    mouse_y = gfx_height() / 2;
}

void mouse_handler(void)
{
    uint8_t status = inb(PS2_STATUS);
    if (!(status & 0x01))
        return;

    uint8_t b = inb(PS2_DATA);
    uint8_t dx = inb(PS2_DATA);
    uint8_t dy = inb(PS2_DATA);

    mouse_buttons = b & 0x07;

    mouse_x += (int8_t)dx;
    mouse_y -= (int8_t)dy;

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= gfx_width()) mouse_x = gfx_width() - 1;
    if (mouse_y >= gfx_height()) mouse_y = gfx_height() - 1;
}

int mouse_get_x(void) { return mouse_x; }
int mouse_get_y(void) { return mouse_y; }
uint8_t mouse_get_buttons(void) { return mouse_buttons; }
