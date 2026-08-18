#include "boot_logo.h"
static int BS_W, BS_H;

static void bs_delay(int n) { for (volatile int d = 0; d < n; d++); }

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
}

static void bs_fill_circle_aa(int cx, int cy, int r, unsigned c) {
    gfx_fill_circle_aa(cx, cy, r, c);
}

static void bs_gradient_bg(void) {
    int hc_x = BS_W / 2, hc_y = BS_H / 2;
    for (int y = 0; y < BS_H; y++) {
        for (int x = 0; x < BS_W; x += 2) {
            int dx = x - hc_x, dy = y - hc_y;
            int dist = (dx*dx + dy*dy) * 200 / (hc_x*hc_x + hc_y*hc_y);
            if (dist > 200) dist = 200;
            int b = 24 - dist / 10;
            int g = 10 - dist / 22;
            int r = 6 - dist / 34;
            if (b < 2) b = 2; if (g < 0) g = 0; if (r < 0) r = 0;
            gfx_rect(x, y, 2, 1, (r << 16) | (g << 8) | b);
        }
    }
}

static void bs_linux_terminal_boot(void) {
    int cw = BS_W, ch = BS_H;

    // Dark background
    gfx_fill_round_rect(0, 0, cw, ch, 0, 0x080808);

    // Title bar
    int tb_h = 40;
    gfx_fill_round_rect(0, 0, cw, tb_h, 0, 0x1A1A2E);
    gfx_rect(0, tb_h, cw, 1, 0x2A2A4E);
    gfx_print_scaled(cw / 2 - 148, 10, 0x33FF55, "Linux Terminal Boot", 2);

    // Container
    int px = 40, py = 60;
    int pw = cw - 80, ph = ch - 100;
    gfx_fill_round_rect(px + 2, py + 2, pw, ph, 8, 0x000000);
    gfx_fill_round_rect(px, py, pw, ph, 8, 0x0C1A0C);
    gfx_round_rect(px, py, pw, ph, 8, 0x1A4A1A);

    // Scrolling green terminal text
    const char *lines[] = {
        "PixelOS v0.1.0 (bare-metal x86_64)",
        "",
        "[    0.000000] Booting PixelOS kernel...",
        "[    0.000124] CPU: Intel/AMD x86_64 Long Mode",
        "[    0.000256] VGA: VBE 1280x720x32 @ 0xFD000000",
        "[    0.000400] RAM: 256 MB available",
        "[    0.000512] GDT: Global Descriptor Table loaded",
        "[    0.000600] IDT: Interrupt Descriptor Table ready",
        "[    0.000720] TSS: Task State Segment configured",
        "[    0.000840] PIC: 8259A remapped to IRQ 32",
        "[    0.001000] PS/2: Keyboard driver initialized",
        "[    0.001200] PS/2: Mouse driver initialized (scroll OK)",
        "[    0.001400] PCI: VGA device at 0x70000",
        "[    0.001600] AC97: Audio controller found on PCI bus",
        "[    0.001800] HEAP: Kernel heap at 16 MB, 16-byte aligned",
        "[    0.002000] PAGE: Identity-mapped first 32 MB",
        "[    0.002200] FB:  Framebuffer mapped at 0xFD000000",
        "[    0.002400] TTF: Material Icons font loaded (348 KB)",
        "[    0.002600] NET: Network driver polling enabled",
        "[    0.003000] All drivers initialized successfully.",
        "",
        "PixelOS login: root",
        "Password: ********",
        "",
        "Welcome to PixelOS v0.1.0!",
        "Type 'help' for available commands.",
        "",
        "root@pixelos:~$ uname -a",
        "PixelOS 0.1.0 bare-metal x86_64 GNU/Pixel",
        "root@pixelos:~$ uptime",
        " 00:00:03 up 3 sec,  1 user,  load average: 0.00",
        "root@pixelos:~$ ls /",
        "bin  boot  dev  etc  home  lib  mnt  opt  proc  root  sbin  sys  tmp  usr  var",
        "root@pixelos:~$ cat /proc/cpuinfo",
        "Processor: x86_64 (Long Mode)",
        "BogoMIPS: 2400.00",
        "Features: sse sse2 fpu",
        "",
        "[    0.005000] SYSTEM READY.",
    };
    int total_lines = sizeof(lines) / sizeof(lines[0]);
    int line_h = 16;
    int vis_lines = ph / line_h;
    int scroll_offset = 0;
    int max_scroll = total_lines - vis_lines;
    if (max_scroll < 0) max_scroll = 0;

    // Animate: auto-scroll the green text line by line
    for (scroll_offset = 0; scroll_offset <= max_scroll; scroll_offset++) {
        // Restore container background
        gfx_fill_round_rect(px + 1, py + 1, pw - 2, ph - 2, 7, 0x0C1A0C);

        // Draw visible lines
        for (int i = 0; i < vis_lines && (i + scroll_offset) < total_lines; i++) {
            int li = i + scroll_offset;
            int ly = py + 12 + i * line_h;
            if (ly + line_h > py + ph) break;

            const char *line = lines[li];
            if (!line[0]) continue;

            // Color: bracket timestamps dim green, login prompts bright green
            uint32_t col = 0x33FF55;
            if (line[0] == '[') col = 0x22AA44;
            else if (line[0] == 'P' && line[1] == 'i') col = 0x44FF66;
            else if (line[0] == 'r' && line[1] == 'o') col = 0x00FF00;
            else if (line[0] == 'W') col = 0x44FF66;

            gfx_print(px + 16, ly, col, line);
        }

        // Cursor blink at bottom
        int cur_y = py + 12 + (vis_lines < total_lines ? vis_lines - 1 : total_lines - 1 - scroll_offset) * line_h;
        if (scroll_offset < max_scroll) {
            int cursor_blink = (scroll_offset / 2) % 2;
            if (cursor_blink) {
                const char *last = lines[total_lines - 1];
                int tw = 0; while (last[tw]) tw++;
                gfx_fill_round_rect(px + 16 + tw * 8, cur_y, 8, 14, 1, 0x33FF55);
            }
        }

        bs_delay(6000000);
    }

    // Final pause
    bs_delay(12000000);
}

void boot_screen(int scr_w, int scr_h) {
    BS_W = scr_w; BS_H = scr_h;
    int cx = scr_w / 2, cy = scr_h * 3 / 8;

    bs_gradient_bg();

    // ─── Logo: layered ring with glow ───
    bs_fill_circle(cx, cy, 56, 0x081020);
    bs_fill_circle(cx, cy, 52, 0x0E1E3A);
    bs_fill_circle(cx, cy, 48, 0x142850);
    bs_fill_circle(cx, cy, 44, 0x1A3268);
    bs_fill_circle(cx, cy, 40, 0x142850);
    bs_fill_circle(cx, cy, 36, 0x0E1E3A);

    // K letter in center
    { int lx = cx - 12, ly = cy - 15;
      gfx_rect(lx, ly, 4, 30, 0x88BBFF);
      for (int i = 0; i < 15; i++) {
          gfx_rect(lx + 4 + i, ly + 13 - i, 2, 3, 0x88BBFF);
      }
      for (int i = 0; i < 15; i++) {
          gfx_rect(lx + 4 + i, ly + 16 + i, 2, 3, 0x88BBFF);
      }
    }

    // ─── Title ───
    int tw = 16 * 7;
    gfx_print_scaled(cx - tw/2 + 2, cy + 68, 0x0A1430, "KairoOS", 3);
    gfx_print_scaled(cx - tw/2, cy + 66, 0x88BBFF, "KairoOS", 3);
    gfx_print(cx - 36, cy + 98, 0x3A5A8A, "v0.1.0");

    // Dot positions around the logo (8 positions on ellipse)
    static const int dot_dx[8] = {0, 22, 41, 54, 58, 54, 41, 22};
    static const int dot_dy[8] = {56, 52, 40, 22, 0, -22, -40, -52};

    for (int p = 0; p <= 60; p++) {
        // ─── Orbiting dots ───
        int dot_idx = (p / 2) % 8;
        for (int d = 0; d < 3; d++) {
            int di = (dot_idx + d * 3) % 8;
            int sz = 4 - d;
            int xx = cx + dot_dx[di], yy = cy - dot_dy[di];
            int bright = 0xAA - d * 0x30;
            unsigned col = (bright << 16) | ((bright * 3 / 4) << 8) | (bright * 5 / 4);
            bs_fill_circle_aa(xx, yy, sz, col);
        }

        // ─── Pulse ring ───
        int gp = (p % 8 < 4) ? (p % 8) : (8 - p % 8);
        gfx_circle_aa(cx, cy, 54 + gp, 1, 0x1A3268);

        bs_delay(300000);

        // ─── Erase dots ───
        for (int d = 0; d < 3; d++) {
            int di = (dot_idx + d * 3) % 8;
            int xx = cx + dot_dx[di], yy = cy - dot_dy[di];
            int ddx = xx - cx, ddy = yy - cy;
            int dist = (ddx*ddx + ddy*ddy) * 200 / (cx*cx + cy*cy);
            if (dist > 200) dist = 200;
            int b = 24 - dist / 10;
            int g = 10 - dist / 22;
            int r = 6 - dist / 34;
            if (b < 2) b = 2; if (g < 0) g = 0; if (r < 0) r = 0;
            bs_fill_circle(xx, yy, 6, (r << 16) | (g << 8) | b);
        }
        gfx_circle_aa(cx, cy, 54 + gp, 1, 0x000000);
    }

    // ─── Fade to Linux Terminal Boot ───
    for (int f = 0; f < 15; f++) {
        int a = f * 17;
        gfx_rect_alpha(0, 0, scr_w, scr_h, 0x000000, a);
        bs_delay(800000);
    }

    // ─── Linux Terminal Boot screen ───
    bs_linux_terminal_boot();

    // ─── "Powered by" logo ───
    {
        gfx_clear(0x000000);
        int lx = (scr_w - LOGO_W) / 2, ly = scr_h - LOGO_H - 20;
        gfx_print((scr_w - 90) / 2, ly - 14, 0x2A3A5A, "Powered by");
        for (int yy = 0; yy < LOGO_H; yy++)
            for (int xx = 0; xx < LOGO_W; xx++) {
                uint32_t c = logo_data[yy * LOGO_W + xx];
                if (c) gfx_putpixel(lx + xx, ly + yy, c);
            }
        bs_delay(15000000);
    }

    // ─── Fade out ───
    for (int f = 0; f < 20; f++) {
        int a = f * 12;
        gfx_rect_alpha(0, 0, scr_w, scr_h, 0x000000, a);
        bs_delay(1200000);
    }
    gfx_clear(0x000000);
}
