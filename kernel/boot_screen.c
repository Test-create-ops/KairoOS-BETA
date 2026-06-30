static int BS_W, BS_H;

static void bs_FillScreen(unsigned c) { gfx_clear(c); }
static void bs_DrawRect(int x, int y, int w, int h, unsigned c) { gfx_rect(x, y, w, h, c); }

static void bs_DrawCircle(int x, int y, int r, unsigned c) {
    int cx = 0, cy = r, d = 3 - 2 * r;
    while (cx <= cy) {
        gfx_putpixel(x+cx, y+cy, c); gfx_putpixel(x-cy, y-cx, c);
        gfx_putpixel(x+cy, y+cx, c); gfx_putpixel(x-cx, y-cy, c);
        gfx_putpixel(x-cx, y+cy, c); gfx_putpixel(x+cy, y-cx, c);
        gfx_putpixel(x+cx, y-cy, c); gfx_putpixel(x-cy, y+cx, c);
        if (d < 0) d += 4 * cx + 6;
        else { d += 4 * (cx - cy) + 10; cy--; }
        cx++;
    }
}
static void bs_DrawText(const char *t, int x, int y, unsigned c) { gfx_print(x, y, c, t); }
static int bs_CenterX(int w) { return (BS_W - w) / 2; }

void boot_screen(int scr_w, int scr_h) {
    BS_W = scr_w; BS_H = scr_h;

    bs_FillScreen(0x000000);

    int cx = scr_w / 2, cy = scr_h * 2 / 5;

    // Logo: white circle ring with stylized O
    for (int r = 48; r >= 42; r--)
        bs_DrawCircle(cx, cy, r, 0xFFFFFF);
    bs_DrawCircle(cx, cy, 38, 0xFFFFFF);
    bs_DrawText("O", cx - 6, cy - 10, 0xFFFFFF);

    // Title
    bs_DrawText("KairoOS", bs_CenterX(72), cy + 70, 0xFFFFFF);
    bs_DrawText("Version 1.0", bs_CenterX(82), cy + 90, 0x888888);

    // Loading bar outline + fill animation
    int bx = bs_CenterX(300), by = cy + 125, bw = 300, bh = 4;
    bs_DrawRect(bx, by, bw, bh, 0x222222);

    for (int p = 0; p <= 100; p += 2) {
        if (p > 0)
            bs_DrawRect(bx, by, bw * p / 100, bh, 0xFFFFFF);
        for (volatile int d = 0; d < 2000000; d++);
    }

    // Footer
    bs_DrawText("Made in DevEco, published in Aptoide",
                bs_CenterX(284), scr_h - 30, 0x555555);
}
