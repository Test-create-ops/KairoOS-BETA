#include "boot_logo.h"
static int BS_W, BS_H;

static void bs_rect(int x, int y, int w, int h, unsigned c) { gfx_rect(x, y, w, h, c); }
static void bs_round(int x, int y, int w, int h, int r, unsigned c) { gfx_fill_round_rect(x, y, w, h, r, c); }

static void bs_fill_circle(int cx, int cy, int r, unsigned c) {
    int x = 0, y = r, d = 3 - 2*r;
    while (x <= y) {
        gfx_rect(cx-x, cy-y, x*2+1, 1, c);
        gfx_rect(cx-x, cy+y, x*2+1, 1, c);
        gfx_rect(cx-y, cy-x, y*2+1, 1, c);
        gfx_rect(cx-y, cy+x, y*2+1, 1, c);
        if (d < 0) d += 4*x + 6;
        else { d += 4*(x-y) + 10; y--; }
        x++;
    }
    gfx_rect(cx, cy-r, 1, r*2+1, c);
}

static void bs_circle(int cx, int cy, int r, unsigned c) {
    int x = 0, y = r, d = 3 - 2*r;
    while (x <= y) {
        gfx_putpixel(cx+x, cy+y, c); gfx_putpixel(cx-cy, cy-cx, c);
        gfx_putpixel(cx+cy, cy+cx, c); gfx_putpixel(cx-cx, cy-cy, c);
        gfx_putpixel(cx-cx, cy+cy, c); gfx_putpixel(cx+cy, cy-cx, c);
        gfx_putpixel(cx+cx, cy-cy, c); gfx_putpixel(cx-cy, cy+cx, c);
        if (d < 0) d += 4*x + 6;
        else { d += 4*(x-y) + 10; y--; }
        x++;
    }
}

static void bs_delay(int n) { for (volatile int d = 0; d < n; d++); }

// 8 positions for dots on a circle
static int dot_x[8] = {0, 22, 41, 54, 58, 54, 41, 22};
static int dot_y[8] = {58, 54, 41, 22, 0, -22, -41, -54};

static void bs_gradient_bg(void) {
    for (int y = 0; y < BS_H; y++) {
        int t = y * 255 / BS_H;
        int r = 1 + t/30, g = 2 + t/40, b = 6 + t/20;
        if (r > 8) r = 8; if (g > 12) g = 12; if (b > 30) b = 30;
        gfx_rect(0, y, BS_W, 1, (r<<16)|(g<<8)|b);
    }
}

void boot_screen(int scr_w, int scr_h) {
    BS_W = scr_w; BS_H = scr_h;
    int cx = scr_w / 2, cy = scr_h * 3 / 8;

    bs_gradient_bg();

    // Logo: multi-ring
    bs_fill_circle(cx, cy, 54, 0x4488FF);
    bs_fill_circle(cx, cy, 50, 0x3366CC);
    bs_fill_circle(cx, cy, 46, 0x224488);
    bs_fill_circle(cx, cy, 42, 0x1A3366);
    bs_fill_circle(cx, cy, 38, 0x112244);

    // V letter in center — proper V that closes at bottom
    for (int dy = -20; dy <= 0; dy++) {
        int dx = (-dy * 16 / 20);
        gfx_rect(cx-dx-3, cy+dy, 4, 2, 0xAADDFF);
        gfx_rect(cx+dx-1, cy+dy, 4, 2, 0xAADDFF);
    }

    // Title
    int tx = (scr_w - 128) / 2;
    gfx_print_scaled(tx, cy + 70, 0x88CCFF, "Viteza", 2);
    gfx_print((scr_w - 82) / 2, cy + 94, 0x556688, "Version 1.0");

    // Progress bar
    int bx = (scr_w - 300) / 2, by = cy + 140, bw = 300, bh = 3;
    bs_round(bx, by, bw, bh, 1, 0x1A2A4A);

    const char *msgs[] = { "Initializing", "Loading kernel", "Starting services", "Almost ready" };
    int prev_msg = -1, prev_fill = -1;

    for (int p = 0; p <= 100; p++) {
        // Rotating dots around logo (8 positions)
        int dot_idx = (p / 2) % 8;
        for (int d = 0; d < 4; d++) {
            int di = (dot_idx + d * 2) % 8;
            int sz = d == 0 ? 7 : 5;
            int x = cx + dot_x[di], y = cy + dot_y[di];
            unsigned col = d == 0 ? 0x66CCFF : 0x3366AA;
            bs_round(x - sz/2, y - sz/2, sz, sz, sz/2, col);
        }

        // Radial glow pulse
        int gp = (p % 10 < 5) ? (p % 10) : (10 - (p % 10));
        int gr = 40 + gp * 2;
        bs_circle(cx, cy, gr+1, 0x4488FF);
        bs_delay(500000);
        bs_circle(cx, cy, gr+1, 0x000000);

        // Progress bar with gradient-like fill
        int fill = bw * p / 100;
        if (fill != prev_fill) {
            // Fill the bar
            for (int x = bx; x < bx + fill; x += 2) {
                int dist = x - bx;
                int r = 0x44 + dist/10; if (r > 0xAA) r = 0xAA;
                gfx_rect(x, by, 2, bh, (r<<16)|((r*3/4)<<8)|0xFF);
            }
            // Glow tip
            if (fill > 4) {
                bs_round(bx+fill-6, by-2, 10, bh+4, 3, 0x4488FF);
                bs_round(bx+fill-3, by-1, 6, bh+2, 2, 0x88CCFF);
            }
            prev_fill = fill;
        }

        // Status message
        int msg_idx = p < 30 ? 0 : p < 55 ? 1 : p < 80 ? 2 : 3;
        if (msg_idx != prev_msg) {
            prev_msg = msg_idx;
        }
        // Animated dots
        int dots = (p / 4) % 4;
        char st[32]; int si = 0;
        for (int i = 0; msgs[msg_idx][i]; i++) st[si++] = msgs[msg_idx][i];
        st[si++] = '.'; if (dots > 0) { st[si++] = '.'; if (dots > 1) st[si++] = '.'; }
        st[si] = 0;
        bs_rect((scr_w-120)/2, by+14, 120, 14, 0x000000);
        // Restore gradient behind text
        for (int yy = by+14; yy < by+28; yy++) {
            int t = yy * 255 / BS_H;
            int r = 1 + t/30, g = 2 + t/40, b = 6 + t/20;
            if (r > 8) r = 8; if (g > 12) g = 12; if (b > 30) b = 30;
            gfx_rect((scr_w-120)/2, yy, 120, 1, (r<<16)|(g<<8)|b);
        }
        gfx_print((scr_w-100)/2, by+14, 0x556688, st);

        // Sparkle particles
        for (int sp = 0; sp < 3; sp++) {
            int sx = ((p*13 + sp*37) * 7 + 11) % scr_w;
            int sy = ((p*17 + sp*53) * 11 + 7) % (scr_h * 2 / 3);
            int sb = ((p*3 + sp*7) % 8);
            if (sb < 3) continue;
            unsigned sc = sb > 5 ? 0x88CCFF : 0x5588BB;
            gfx_rect(sx, sy, 2, 2, sc);
        }

        bs_delay(10000000);

        // Erase arc dots
        for (int d = 0; d < 4; d++) {
            int di = (dot_idx + d * 2) % 8;
            int x = cx + dot_x[di], y = cy + dot_y[di];
            bs_round(x-4, y-4, 8, 8, 4, 0x000000);
            bs_round(x-3, y-3, 6, 6, 3, 0x000000);
            // Restore gradient
            for (int yy = y-4; yy < y+4; yy++) {
                if (yy < 0 || yy >= BS_H) continue;
                int t = yy * 255 / BS_H;
                int r = 1 + t/30, g = 2 + t/40, b = 6 + t/20;
                if (r > 8) r = 8; if (g > 12) g = 12; if (b > 30) b = 30;
                gfx_rect(x-4, yy, 8, 1, (r<<16)|(g<<8)|b);
            }
        }
    }

    // "Powered by" logo — bottom of screen
    {
        int lx = (scr_w - LOGO_W) / 2, ly = scr_h - LOGO_H - 20;
        gfx_print((scr_w - 90) / 2, ly - 14, 0x445566, "Powered by");
        for (int yy = 0; yy < LOGO_H; yy++) {
            for (int xx = 0; xx < LOGO_W; xx++) {
                uint32_t c = logo_data[yy * LOGO_W + xx];
                if (c) gfx_putpixel(lx + xx, ly + yy, c);
            }
        }
    }

    // Fade out
    for (int f = 0; f < 15; f++) {
        int a = f * 16;
        for (int y = 0; y < scr_h; y += 2) {
            gfx_rect(0, y, scr_w, 2, (a<<16)|(a<<8)|a);
        }
        bs_delay(2000000);
    }
    gfx_clear(0x000000);
}
