#include "mouse.h"
#include "../graphics/gfx.h"
#include "../usb/usb.h"

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define CURSOR_W 9
#define CURSOR_H 13

static int mouse_x = 100;
static int mouse_y = 100;
static uint8_t mouse_buttons = 0;
static int mouse_packet_byte = 0;
static uint8_t mouse_packet[3];
static int mouse_btn_prev = 0;
static int ps2_active = 0;
static int ps2_tried = 0;

static int cursor_drawn = 0;
static int last_cx = 0, last_cy = 0;
static uint32_t cursor_bg[CURSOR_H][CURSOR_W];

static const uint16_t cursor_shape[CURSOR_H] = {
    0b100000000,
    0b110000000,
    0b101000000,
    0b100100000,
    0b100010000,
    0b100001000,
    0b100000100,
    0b100000010,
    0b100000001,
    0b111111111,
    0b100000100,
    0b100001000,
    0b100010000,
};

void mouse_init(void)
{
    mouse_x = 100;
    mouse_y = 100;
    mouse_buttons = 0;
    mouse_packet_byte = 0;
    ps2_active = 0;
    ps2_tried = 0;
}

static void ps2_try_init(void)
{
    if (ps2_tried) return;
    ps2_tried = 1;

    // Enable PS/2 auxiliary port
    outb(PS2_STATUS, 0xA8);
    for (volatile int d = 0; d < 5000; d++);

    // Read command byte, enable both keyboard + mouse IRQs
    outb(PS2_STATUS, 0x20);
    for (volatile int d = 0; d < 5000; d++);
    if (inb(PS2_STATUS) & 1) {
        uint8_t cmd = inb(PS2_DATA);
        cmd |= 0x03;
        for (volatile int d = 0; d < 5000; d++);
        outb(PS2_STATUS, 0x60);
        for (volatile int d = 0; d < 5000; d++);
        outb(PS2_DATA, cmd);
    }

    // Set mouse defaults
    for (volatile int d = 0; d < 5000; d++);
    outb(PS2_STATUS, 0xD4);
    for (volatile int d = 0; d < 5000; d++);
    outb(PS2_DATA, 0xF6);
    for (volatile int d = 0; d < 50000; d++);

    // Enable data reporting
    outb(PS2_STATUS, 0xD4);
    for (volatile int d = 0; d < 5000; d++);
    outb(PS2_DATA, 0xF4);

    ps2_active = 1;
}

static void process_packet(void)
{
    mouse_btn_prev = mouse_buttons;
    mouse_buttons = mouse_packet[0] & 0x07;

    int dx = (int)(int8_t)mouse_packet[1];
    int dy = (int)(int8_t)mouse_packet[2];

    mouse_x += dx;
    mouse_y -= dy;

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= gfx_width()) mouse_x = gfx_width() - 1;
    if (mouse_y >= gfx_height()) mouse_y = gfx_height() - 1;
}

void mouse_poll(void)
{
    // Try USB HID tablet first (works on macOS QEMU with usb-tablet)
    int ux, uy, ubtn;
    if (usb_hid_read(&ux, &uy, &ubtn)) {
        // Scale absolute 0-32767 to screen size
        if (gfx_width() > 0) mouse_x = ux * gfx_width() / 32768;
        if (gfx_height() > 0) mouse_y = uy * gfx_height() / 32768;
        mouse_btn_prev = mouse_buttons;
        mouse_buttons = ubtn;
        return;
    }

    // Lazy PS/2 init
    if (!ps2_tried) ps2_try_init();

    // If PS/2 is active, poll it
    if (ps2_active) {
        while (1) {
            uint8_t s = inb(PS2_STATUS);
            if (!(s & 1)) break;
            if (!(s & 0x20)) {  // not mouse data, leave it for keyboard
                uint8_t kdata = inb(PS2_DATA);
                break;
            }
            uint8_t data = inb(PS2_DATA);

            if (mouse_packet_byte == 0) {
                if (!(data & 0x08)) continue;
                mouse_packet[0] = data;
                mouse_packet_byte = 1;
            } else if (mouse_packet_byte == 1) {
                mouse_packet[1] = data;
                mouse_packet_byte = 2;
            } else {
                mouse_packet[2] = data;
                mouse_packet_byte = 0;
                process_packet();
            }
        }
    }
}

// Keyboard-driven cursor movement
void mouse_move_by_key(char k)
{
    int step = 8;
    if (k == 0x01) mouse_y -= step;  // UP
    if (k == 0x02) mouse_y += step;  // DOWN
    if (k == 0x03) mouse_x -= step;  // LEFT
    if (k == 0x04) mouse_x += step;  // RIGHT
    if (k == ' ') { mouse_btn_prev = mouse_buttons; mouse_buttons = 1; }

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= gfx_width()) mouse_x = gfx_width() - 1;
    if (mouse_y >= gfx_height()) mouse_y = gfx_height() - 1;
}

void draw_cursor(void)
{
    // Erase old cursor
    if (cursor_drawn) {
        for (int i = 0; i < CURSOR_H; i++) {
            for (int j = 0; j < CURSOR_W; j++) {
                int x = last_cx + j, y = last_cy + i;
                if (x >= 0 && x < gfx_width() && y >= 0 && y < gfx_height())
                    gfx_putpixel(x, y, cursor_bg[i][j]);
            }
        }
    }

    // Save background at new position
    for (int i = 0; i < CURSOR_H; i++) {
        for (int j = 0; j < CURSOR_W; j++) {
            int x = mouse_x + j, y = mouse_y + i;
            if (x >= 0 && x < gfx_width() && y >= 0 && y < gfx_height())
                cursor_bg[i][j] = gfx_getpixel(x, y);
        }
    }

    // Draw cursor at new position
    for (int i = 0; i < CURSOR_H; i++) {
        for (int j = 0; j < CURSOR_W; j++) {
            if (cursor_shape[i] & (1 << (8-j))) {
                int x = mouse_x + j, y = mouse_y + i;
                if (x >= 0 && x < gfx_width() && y >= 0 && y < gfx_height())
                    gfx_putpixel(x, y, 0xFFFFFF);
            }
        }
    }

    last_cx = mouse_x;
    last_cy = mouse_y;
    cursor_drawn = 1;
}

int mouse_clicked(void)
{
    return (mouse_buttons & 1) && !(mouse_btn_prev & 1);
}

int mouse_get_x(void) { return mouse_x; }
int mouse_get_y(void) { return mouse_y; }
uint8_t mouse_get_buttons(void) { return mouse_buttons; }
