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
static void bs_Delay(int n) { for (volatile int d = 0; d < n; d++); }

void boot_screen(int scr_w, int scr_h) {
    BS_W = scr_w; BS_H = scr_h;
    int cx = scr_w / 2, cy = scr_h * 2 / 5;

    bs_FillScreen(0x000000);

    // White ring with K
    for (int r = 50; r >= 42; r--)
        bs_DrawCircle(cx, cy, r, 0xFFFFFF);
    bs_DrawCircle(cx, cy, 38, 0xFFFFFF);
    bs_DrawText("V", cx - 7, cy - 10, 0xFFFFFF);

    // Title
    int tx = bs_CenterX(64);
    bs_DrawText("Viteza", tx, cy + 70, 0xFFFFFF);
    bs_DrawText("Version 1.0", bs_CenterX(82), cy + 90, 0x888888);

    // Progress bar outline
    int bx = bs_CenterX(300), by = cy + 125, bw = 300, bh = 4;
    bs_DrawRect(bx, by, bw, bh, 0x222222);

    const char *msgs[] = { "Initializing...", "Loading kernel...", "Starting services..." };
    int prev_msg = -1, prev_fill = -1;

    // Color rotation table for the title
    unsigned colors[] = { 0xFFFFFF, 0x4488FF, 0xFF4488, 0x88FF44, 0xFFAA00, 0xBB88FF };

    for (int p = 0; p <= 100; p++) {
        // Glow pulse ring (expanding blue ring)
        int pulse = p % 10;
        int pr = 52 + pulse;
        bs_DrawCircle(cx, cy, pr, 0x4488FF);
        bs_DrawCircle(cx, cy, pr - 1, 0x4488FF);

        // Color-cycling Viteza title
        int ci = (p / 4) % 6;
        bs_DrawRect(tx, cy + 70, 64, 8, 0x000000);
        bs_DrawText("Viteza", tx, cy + 70, colors[ci]);

        // Progress bar fill
        int fill = bw * p / 100;
        if (fill != prev_fill) {
            bs_DrawRect(bx, by, fill, bh, 0x4488FF);
            if (fill > 4)
                bs_DrawRect(bx + fill - 4, by - 1, 6, bh + 2, 0x66BBFF);
            prev_fill = fill;
        }

        // Status message at thresholds
        int msg_idx = (p < 33) ? 0 : (p < 66) ? 1 : 2;
        if (msg_idx != prev_msg) {
            bs_DrawRect(bs_CenterX(120), by + 14, 120, 14, 0x000000);
            bs_DrawText(msgs[msg_idx], bs_CenterX(120), by + 14, 0x666666);
            prev_msg = msg_idx;
        }

        // Hold visible
        bs_Delay(15000000);

        // Erase pulse ring
        bs_DrawCircle(cx, cy, pr, 0x000000);
        bs_DrawCircle(cx, cy, pr - 1, 0x000000);
    }

    bs_DrawText("Made in DevEco, published in Aptoide",
                bs_CenterX(284), scr_h - 30, 0x555555);
    bs_Delay(3000000);
}
