#include "../../lib/framebuffer.h"

typedef struct {
    unsigned short width;
    unsigned short height;
    unsigned short bpp;
    unsigned int framebuffer;
} vbe_info_t;

static vbe_info_t vbe;

void vbe_init(void)
{
    vbe.width = 1280;
    vbe.height = 720;
    vbe.bpp = 32;
    vbe.framebuffer = 0xFD000000;

    fb_write("VBE grafica inizializzata\n");
}
