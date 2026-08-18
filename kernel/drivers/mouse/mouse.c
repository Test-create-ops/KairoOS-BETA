#include "mouse.h"
#include "../graphics/gfx.h"
#include "../usb/usb.h"
#include "../../cursor_pack.h"

#define PS2_DATA 0x60
#define PS2_STATUS 0x64

static int mouse_x = 100;
static int mouse_y = 100;
static uint8_t mouse_buttons = 0;
static int mouse_packet_byte = 0;
static uint8_t mouse_packet[3];
static int mouse_btn_prev = 0;
static int ps2_active = 0;
static int ps2_tried = 0;
static int mouse_irq_delivered = 0;
static int mouse_scroll_delta = 0;

static int cursor_drawn = 0;
static int last_cx = 0, last_cy = 0;
static uint32_t cursor_bg[48][48];

static enum cursor_type current_cursor = CURSOR_ARROW;
int christmas_cursor = 0;

void mouse_set_cursor(enum cursor_type t) {
    if (t >= 0 && t < CURSOR_COUNT) current_cursor = t;
}

enum cursor_type mouse_get_cursor(void) {
    return current_cursor;
}

void mouse_init(void)
{
    mouse_x = 100;
    mouse_y = 100;
    mouse_buttons = 0;
    mouse_packet_byte = 0;
    ps2_active = 0;
    ps2_tried = 0;
    current_cursor = CURSOR_ARROW;
}

static void ps2_try_init(void)
{
    if (ps2_tried) return;
    ps2_tried = 1;

    outb(PS2_STATUS, 0xA8);
    for (volatile int d = 0; d < 5000; d++);

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

    for (volatile int d = 0; d < 5000; d++);
    outb(PS2_STATUS, 0xD4);
    for (volatile int d = 0; d < 5000; d++);
    outb(PS2_DATA, 0xF6);
    for (volatile int d = 0; d < 50000; d++);

    outb(PS2_STATUS, 0xD4);
    for (volatile int d = 0; d < 5000; d++);
    outb(PS2_DATA, 0xF4);

    ps2_active = 1;
}

static void process_packet(int has_scroll, int scroll)
{
    mouse_btn_prev = mouse_buttons;
    mouse_buttons = mouse_packet[0] & 0x07;

    int dx = (int)(int8_t)mouse_packet[1];
    int dy = (int)(int8_t)mouse_packet[2];

    mouse_x += dx;
    mouse_y -= dy;

    if (has_scroll) mouse_scroll_delta += scroll;

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= gfx_width()) mouse_x = gfx_width() - 1;
    if (mouse_y >= gfx_height()) mouse_y = gfx_height() - 1;
}

void mouse_poll(void)
{
    int ux, uy, ubtn;
    if (usb_hid_read(&ux, &uy, &ubtn)) {
        if (gfx_width() > 0) mouse_x = ux * gfx_width() / 32768;
        if (gfx_height() > 0) mouse_y = uy * gfx_height() / 32768;
        mouse_btn_prev = mouse_buttons;
        mouse_buttons = ubtn;
        return;
    }

    if (!ps2_tried) ps2_try_init();

    if (ps2_active && !mouse_irq_delivered) {
        while (1) {
            uint8_t s = inb(PS2_STATUS);
            if (!(s & 1)) break;
            if (!(s & 0x20)) {
                (void)inb(PS2_DATA);
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
                process_packet(0, 0);
                if (inb(PS2_STATUS) & 1) {
                    uint8_t s2 = inb(PS2_DATA);
                    if (!(s2 & 0x08)) {
                        mouse_scroll_delta += (int)(int8_t)s2;
                    } else {
                        mouse_packet[0] = s2;
                        mouse_packet_byte = 1;
                    }
                }
            }
        }
    }
}

void mouse_handler(void)
{
    uint8_t data = inb(PS2_DATA);
    mouse_irq_delivered = 1;

    if (mouse_packet_byte == 0) {
        if (!(data & 0x08)) return;
        mouse_packet[0] = data;
        mouse_packet_byte = 1;
    } else if (mouse_packet_byte == 1) {
        mouse_packet[1] = data;
        mouse_packet_byte = 2;
    } else {
        mouse_packet[2] = data;
        mouse_packet_byte = 0;
        process_packet(0, 0);
        if (inb(PS2_STATUS) & 1) {
            uint8_t s2 = inb(PS2_DATA);
            if (!(s2 & 0x08)) {
                mouse_scroll_delta += (int)(int8_t)s2;
            } else {
                mouse_packet[0] = s2;
                mouse_packet_byte = 1;
            }
        }
    }
}

void mouse_move_by_key(char k)
{
    int step = 8;
    if (k == 0x01) mouse_y -= step;
    if (k == 0x02) mouse_y += step;
    if (k == 0x03) mouse_x -= step;
    if (k == 0x04) mouse_x += step;
    if (k == ' ') { mouse_btn_prev = mouse_buttons; mouse_buttons = 1; }

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= gfx_width()) mouse_x = gfx_width() - 1;
    if (mouse_y >= gfx_height()) mouse_y = gfx_height() - 1;
}

void draw_cursor(void)
{
    const cursor_entry_t *ce = &cursor_table[current_cursor];
    int cw = ce->w, ch = ce->h;
    int hx = ce->hx, hy = ce->hy;
    const uint32_t *cd = ce->data;

    if (cursor_drawn) {
        for (int i = 0; i < 48; i++) {
            for (int j = 0; j < 48; j++) {
                int x = last_cx + j, y = last_cy + i;
                if (x >= 0 && x < gfx_width() && y >= 0 && y < gfx_height())
                    gfx_putpixel(x, y, cursor_bg[i][j]);
            }
        }
    }

    for (int i = 0; i < 48; i++) {
        for (int j = 0; j < 48; j++) {
            int x = mouse_x - hx + j, y = mouse_y - hy + i;
            if (x >= 0 && x < gfx_width() && y >= 0 && y < gfx_height())
                cursor_bg[i][j] = gfx_getpixel(x, y);
            else
                cursor_bg[i][j] = 0;
        }
    }

    for (int i = 0; i < ch; i++) {
        for (int j = 0; j < cw; j++) {
            uint32_t px = cd[i * cw + j];
            uint8_t a = (px >> 24) & 0xFF;
            if (a == 0) continue;

            int x = mouse_x - hx + j;
            int y = mouse_y - hy + i;
            if (x < 0 || y < 0 || x >= gfx_width() || y >= gfx_height()) continue;

            if (a == 255) {
                gfx_putpixel(x, y, px & 0x00FFFFFF);
            } else {
                uint32_t bg = gfx_getpixel(x, y);
                uint32_t sr = (px >> 16) & 0xFF;
                uint32_t sg = (px >> 8) & 0xFF;
                uint32_t sb = px & 0xFF;
                uint32_t br = (bg >> 16) & 0xFF;
                uint32_t bg2 = (bg >> 8) & 0xFF;
                uint32_t bb = bg & 0xFF;
                uint32_t or2 = (sr * a + br * (255 - a)) / 255;
                uint32_t og = (sg * a + bg2 * (255 - a)) / 255;
                uint32_t ob = (sb * a + bb * (255 - a)) / 255;
                gfx_putpixel(x, y, (or2 << 16) | (og << 8) | ob);
            }
        }
    }

    last_cx = mouse_x;
    last_cy = mouse_y;
    cursor_drawn = 1;
}

void cursor_hide(void)
{
    if (!cursor_drawn) return;
    for (int i = 0; i < 48; i++) {
        for (int j = 0; j < 48; j++) {
            int x = last_cx + j, y = last_cy + i;
            if (x >= 0 && x < gfx_width() && y >= 0 && y < gfx_height())
                gfx_putpixel(x, y, cursor_bg[i][j]);
        }
    }
    cursor_drawn = 0;
}

int mouse_clicked(void)
{
    return (mouse_buttons & 1) && !(mouse_btn_prev & 1);
}

int mouse_get_scroll(void)
{
    int d = mouse_scroll_delta;
    mouse_scroll_delta = 0;
    return d;
}

int mouse_get_x(void) { return mouse_x; }
int mouse_get_y(void) { return mouse_y; }
uint8_t mouse_get_buttons(void) { return mouse_buttons; }
