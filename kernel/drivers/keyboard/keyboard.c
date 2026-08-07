#include "keyboard.h"
#include "../../lib/io.h"

#define PS2_DATA  0x60
#define PS2_CMD   0x64
#define PS2_STAT  0x64

static const char scancode_set1[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0,'\\',
    'z','x','c','v','b','n','m',',','.','/', 0, '*', 0,' ',
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static volatile char last_char = 0;
static volatile int last_char_ctrl = 0;
static int e0_prefix = 0;
static int alt_pressed = 0;
static int ctrl_pressed = 0;

#define KEY_UP    0x01
#define KEY_DOWN  0x02
#define KEY_LEFT  0x03
#define KEY_RIGHT 0x04
#define KEY_HOME  0x05
#define KEY_END   0x06
#define KEY_PGUP  0x07
#define KEY_PGDN  0x08

char keyboard_last_char(void)
{
    char c = last_char;
    last_char = 0;
    return c;
}

int keyboard_last_ctrl(void)
{
    int c = last_char_ctrl;
    last_char_ctrl = 0;
    return c;
}

void keyboard_handler(void)
{
    uint8_t sc = inb(PS2_DATA);

    if (sc == 0xE0) {
        e0_prefix = 1;
        return;
    }

    if (e0_prefix) {
        e0_prefix = 0;
        if (sc == 0x1D) { ctrl_pressed = 1; return; }
        if (sc == 0x9D) { ctrl_pressed = 0; return; }
        if (sc == 0x38) { alt_pressed = 1; return; }
        if (sc == 0xB8) { alt_pressed = 0; return; }
        if (sc & 0x80) return;
        if (sc == 0x48) { last_char = KEY_UP; return; }
        if (sc == 0x50) { last_char = KEY_DOWN; return; }
        if (sc == 0x4B) { last_char = KEY_LEFT; return; }
        if (sc == 0x4D) { last_char = KEY_RIGHT; return; }
        if (sc == 0x47) { last_char = KEY_HOME; return; }
        if (sc == 0x4F) { last_char = KEY_END; return; }
        if (sc == 0x49) { last_char = KEY_PGUP; return; }
        if (sc == 0x51) { last_char = KEY_PGDN; return; }
        return;
    }

    if (sc == 0x1D) { ctrl_pressed = 1; return; }
    if (sc == 0x9D) { ctrl_pressed = 0; return; }
    if (sc == 0x38) { alt_pressed = 1; return; }
    if (sc == 0xB8) { alt_pressed = 0; return; }
    if (sc & 0x80) return;

    char c = scancode_set1[sc];
    if (c) {
        last_char = c;
        last_char_ctrl = ctrl_pressed;
    }
}

static void ps2_wait_write(void)
{
    for (int i = 0; i < 10000; i++) {
        if (!(inb(PS2_STAT) & 2)) return;
    }
}

static void ps2_wait_read(void)
{
    for (int i = 0; i < 10000; i++) {
        if (inb(PS2_STAT) & 1) return;
    }
}

int is_ctrl_pressed(void)
{
    return ctrl_pressed;
}

int is_alt_pressed(void)
{
    return alt_pressed;
}

void keyboard_init(void)
{
    // Enable first PS/2 port (keyboard)
    ps2_wait_write();
    outb(PS2_CMD, 0xAE);

    // Read configuration byte, set bit 0 (enable IRQ)
    ps2_wait_write();
    outb(PS2_CMD, 0x20);
    ps2_wait_read();
    uint8_t cfg = inb(PS2_DATA);
    cfg |= 1;
    ps2_wait_write();
    outb(PS2_CMD, 0x60);
    ps2_wait_write();
    outb(PS2_DATA, cfg);
}
