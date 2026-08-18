#include "gfx.h"
#include "../../../ui/renderer/renderer.h"

static fb_info_t fb;

volatile uint32_t *gfx_get_fb_addr(void) { return fb.framebuffer; }
uint32_t gfx_get_pitch(void) { return fb.pitch; }

void gfx_init(fb_info_t *info)
{
    fb = *info;
}

void gfx_fillrect(int x, int y, int w, int h, uint32_t color)
{
    gfx_rect(x, y, w, h, color);
}

void gfx_drawtext(int x, int y, uint32_t color, const char *text)
{
    gfx_print(x, y, color, text);
}

int gfx_width(void) { return (int)fb.width; }
int gfx_height(void) { return (int)fb.height; }

uint32_t gfx_getpixel(int x, int y)
{
    if (x < 0 || y < 0 || x >= (int)fb.width || y >= (int)fb.height)
        return 0;
    uint32_t *pixel = (uint32_t *)((uint8_t*)fb.framebuffer + y * fb.pitch + x * 4);
    return *pixel;
}

void gfx_putpixel(int x, int y, uint32_t color)
{
    if (x < 0 || y < 0 || x >= (int)fb.width || y >= (int)fb.height)
        return;

    uint32_t *pixel = (uint32_t *)((uint8_t*)fb.framebuffer + y * fb.pitch + x * 4);
    *pixel = color;
}

void gfx_clear(uint32_t color)
{
    for (uint32_t i = 0; i < fb.width * fb.height; i++) {
        fb.framebuffer[i] = color;
    }
}

void gfx_rect(int x, int y, int w, int h, uint32_t color)
{
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            gfx_putpixel(xx, yy, color);
        }
    }
}

void gfx_round_rect(int x, int y, int w, int h, int r, uint32_t color)
{
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    gfx_rect(x+r, y, w-2*r, 1, color);
    gfx_rect(x+r, y+h-1, w-2*r, 1, color);
    gfx_rect(x, y+r, 1, h-2*r, color);
    gfx_rect(x+w-1, y+r, 1, h-2*r, color);
    // simple pixel-perfect quarter arcs
    for (int dy = 0; dy <= r; dy++) {
        int dx = (int)(r - 0.5 + 0.5 * (double)(r*r - dy*dy > 0 ? 1 : 0));
        while (dx*dx + dy*dy > r*r) dx--;
        while ((dx+1)*(dx+1) + dy*dy <= r*r) dx++;
        if (dx < 0) continue;
        gfx_putpixel(x + r - dx, y + r - dy, color);
        gfx_putpixel(x + w - 1 - r + dx, y + r - dy, color);
        gfx_putpixel(x + r - dx, y + h - 1 - r + dy, color);
        gfx_putpixel(x + w - 1 - r + dx, y + h - 1 - r + dy, color);
        // also fill horizontal lines for thick rounded look
        if (color != 0xFFFFFFFF) continue; // skip fill for outline mode
    }
}

void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint32_t color)
{
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    gfx_rect(x, y+r, w, h-2*r, color);
    gfx_rect(x+r, y, w-2*r, r, color);
    gfx_rect(x+r, y+h-r, w-2*r, r, color);
    for (int dy = 0; dy <= r; dy++) {
        int dx = 0;
        while ((dx+1)*(dx+1) + dy*dy <= r*r) dx++;
        gfx_rect(x + r - dx, y + r - dy, dx+1, 1, color);
        gfx_rect(x + w - 1 - r, y + r - dy, dx+1, 1, color);
        gfx_rect(x + r - dx, y + h - 1 - r + dy, dx+1, 1, color);
        gfx_rect(x + w - 1 - r, y + h - 1 - r + dy, dx+1, 1, color);
    }
}

// ─── Anti-aliased rounded rectangle (subpixel coverage) ────────────────

#define AA_SS 6

static int aa_round_inside(int dx, int dy, int w, int h, int r, int sx, int sy)
{
    int px = dx * AA_SS + sx + 1;
    int py = dy * AA_SS + sy + 1;
    int ex = w * AA_SS - 1 - px;
    int by = h * AA_SS - 1 - py;
    int R = r * AA_SS;
    if (px < R && py < R) { int cxx = px - R, cyy = py - R; if (cxx*cxx + cyy*cyy > R*R) return 0; }
    if (ex < R && py < R) { int cxx = ex - R, cyy = py - R; if (cxx*cxx + cyy*cyy > R*R) return 0; }
    if (px < R && by < R) { int cxx = px - R, cyy = by - R; if (cxx*cxx + cyy*cyy > R*R) return 0; }
    if (ex < R && by < R) { int cxx = ex - R, cyy = by - R; if (cxx*cxx + cyy*cyy > R*R) return 0; }
    return 1;
}

void gfx_fill_round_rect_aa(int x, int y, int w, int h, int r, uint32_t color)
{
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    if (r <= 0) { gfx_rect(x, y, w, h, color); return; }
    int N = AA_SS * AA_SS;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= (int)fb.height) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= (int)fb.width) continue;
            int dx = xx - x, dy = yy - y;
            int ex = (x + w - 1) - xx, by = (y + h - 1) - yy;
            if (dy >= r && by >= r && dx >= r && ex >= r) { gfx_putpixel(xx, yy, color); continue; }
            int cov = 0;
            for (int sy = 0; sy < AA_SS; sy++)
                for (int sx = 0; sx < AA_SS; sx++)
                    if (aa_round_inside(dx, dy, w, h, r, sx, sy)) cov++;
            if (cov == N) gfx_putpixel(xx, yy, color);
            else if (cov > 0) {
                int num = cov;
                uint32_t bg = gfx_getpixel(xx, yy);
                int ar = (color >> 16) & 0xFF, ag = (color >> 8) & 0xFF, ab = color & 0xFF;
                int br = (bg >> 16) & 0xFF, bgn = (bg >> 8) & 0xFF, bb = bg & 0xFF;
                int dr = (ar * num + br * (N - num)) / N;
                int dg = (ag * num + bgn * (N - num)) / N;
                int db = (ab * num + bb * (N - num)) / N;
                gfx_putpixel(xx, yy, (dr << 16) | (dg << 8) | db);
            }
        }
    }
}

// ─── Anti-aliased filled circle ────────────────────────────────────────

void gfx_fill_circle_aa(int cx, int cy, int r, uint32_t color)
{
    if (r <= 0) return;
    int N = AA_SS * AA_SS;
    int R2 = r * r * AA_SS * AA_SS;
    for (int yy = cy - r - 1; yy <= cy + r + 1; yy++) {
        if (yy < 0 || yy >= (int)fb.height) continue;
        for (int xx = cx - r - 1; xx <= cx + r + 1; xx++) {
            if (xx < 0 || xx >= (int)fb.width) continue;
            int cov = 0;
            for (int sy = 0; sy < AA_SS; sy++)
                for (int sx = 0; sx < AA_SS; sx++) {
                    int px = (xx - cx) * AA_SS + sx + 1;
                    int py = (yy - cy) * AA_SS + sy + 1;
                    if (px * px + py * py <= R2) cov++;
                }
            if (cov == N) gfx_putpixel(xx, yy, color);
            else if (cov > 0) {
                int num = cov;
                uint32_t bg = gfx_getpixel(xx, yy);
                int ar = (color >> 16) & 0xFF, ag = (color >> 8) & 0xFF, ab = color & 0xFF;
                int br = (bg >> 16) & 0xFF, bgn = (bg >> 8) & 0xFF, bb = bg & 0xFF;
                int dr = (ar * num + br * (N - num)) / N;
                int dg = (ag * num + bgn * (N - num)) / N;
                int db = (ab * num + bb * (N - num)) / N;
                gfx_putpixel(xx, yy, (dr << 16) | (dg << 8) | db);
            }
        }
    }
}

// ─── Anti-aliased ring (annulus) ───────────────────────────────────────

void gfx_circle_aa(int cx, int cy, int r, int thick, uint32_t color)
{
    if (r <= 0) return;
    int N = AA_SS * AA_SS;
    int ro = r, ri = r - thick;
    if (ri < 1) ri = 1;
    int Ro2 = ro * ro * AA_SS * AA_SS, Ri2 = ri * ri * AA_SS * AA_SS;
    for (int yy = cy - ro - 1; yy <= cy + ro + 1; yy++) {
        if (yy < 0 || yy >= (int)fb.height) continue;
        for (int xx = cx - ro - 1; xx <= cx + ro + 1; xx++) {
            if (xx < 0 || xx >= (int)fb.width) continue;
            int cov = 0;
            for (int sy = 0; sy < AA_SS; sy++)
                for (int sx = 0; sx < AA_SS; sx++) {
                    int px = (xx - cx) * AA_SS + sx + 1;
                    int py = (yy - cy) * AA_SS + sy + 1;
                    int d2 = px * px + py * py;
                    if (d2 <= Ro2 && d2 >= Ri2) cov++;
                }
            if (cov == N) gfx_putpixel(xx, yy, color);
            else if (cov > 0) {
                int num = cov;
                uint32_t bg = gfx_getpixel(xx, yy);
                int ar = (color >> 16) & 0xFF, ag = (color >> 8) & 0xFF, ab = color & 0xFF;
                int br = (bg >> 16) & 0xFF, bgn = (bg >> 8) & 0xFF, bb = bg & 0xFF;
                int dr = (ar * num + br * (N - num)) / N;
                int dg = (ag * num + bgn * (N - num)) / N;
                int db = (ab * num + bb * (N - num)) / N;
                gfx_putpixel(xx, yy, (dr << 16) | (dg << 8) | db);
            }
        }
    }
}

static const uint8_t font_stub[128][8] = {
    ['A'] = {0x18, 0x24, 0x42, 0x42, 0x7E, 0x42, 0x42, 0x00},
    ['B'] = {0x7C, 0x22, 0x22, 0x3C, 0x22, 0x22, 0x7C, 0x00},
    ['C'] = {0x3C, 0x42, 0x40, 0x40, 0x40, 0x42, 0x3C, 0x00},
    ['D'] = {0x78, 0x24, 0x22, 0x22, 0x22, 0x24, 0x78, 0x00},
    ['E'] = {0x7E, 0x40, 0x40, 0x78, 0x40, 0x40, 0x7E, 0x00},
    ['F'] = {0x7E, 0x40, 0x40, 0x78, 0x40, 0x40, 0x40, 0x00},
    ['G'] = {0x3C, 0x42, 0x40, 0x4E, 0x42, 0x42, 0x3C, 0x00},
    ['H'] = {0x42, 0x42, 0x42, 0x7E, 0x42, 0x42, 0x42, 0x00},
    ['I'] = {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00},
    ['J'] = {0x3F, 0x0C, 0x0C, 0x0C, 0x0C, 0x4C, 0x38, 0x00},
    ['K'] = {0x42, 0x44, 0x48, 0x70, 0x48, 0x44, 0x42, 0x00},
    ['L'] = {0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x7E, 0x00},
    ['M'] = {0x42, 0x66, 0x5A, 0x42, 0x42, 0x42, 0x42, 0x00},
    ['N'] = {0x42, 0x62, 0x52, 0x4A, 0x46, 0x42, 0x42, 0x00},
    ['O'] = {0x3C, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C, 0x00},
    ['P'] = {0x7C, 0x42, 0x42, 0x7C, 0x40, 0x40, 0x40, 0x00},
    ['Q'] = {0x3C, 0x42, 0x42, 0x42, 0x4A, 0x44, 0x3A, 0x00},
    ['R'] = {0x7C, 0x42, 0x42, 0x7C, 0x48, 0x44, 0x42, 0x00},
    ['S'] = {0x3C, 0x42, 0x40, 0x3C, 0x02, 0x42, 0x3C, 0x00},
    ['T'] = {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00},
    ['U'] = {0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3C, 0x00},
    ['V'] = {0x42, 0x42, 0x42, 0x42, 0x42, 0x24, 0x18, 0x00},
    ['W'] = {0x42, 0x42, 0x42, 0x42, 0x5A, 0x66, 0x42, 0x00},
    ['X'] = {0x42, 0x42, 0x24, 0x18, 0x24, 0x42, 0x42, 0x00},
    ['Y'] = {0x42, 0x42, 0x24, 0x18, 0x18, 0x18, 0x18, 0x00},
    ['Z'] = {0x7E, 0x02, 0x04, 0x08, 0x16, 0x20, 0x7E, 0x00},
    ['0'] = {0x3C, 0x42, 0x46, 0x4A, 0x52, 0x62, 0x42, 0x3C},
    ['1'] = {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E},
    ['2'] = {0x3C, 0x42, 0x02, 0x0C, 0x30, 0x40, 0x7E, 0x7E},
    ['3'] = {0x3C, 0x42, 0x02, 0x1C, 0x02, 0x42, 0x3C, 0x00},
    ['4'] = {0x0C, 0x14, 0x24, 0x44, 0x7E, 0x04, 0x04, 0x04},
    ['5'] = {0x7E, 0x40, 0x7C, 0x02, 0x02, 0x42, 0x3C, 0x00},
    ['6'] = {0x1C, 0x20, 0x40, 0x7C, 0x42, 0x42, 0x3C, 0x00},
    ['7'] = {0x7E, 0x02, 0x04, 0x08, 0x10, 0x20, 0x20, 0x00},
    ['8'] = {0x3C, 0x42, 0x42, 0x3C, 0x42, 0x42, 0x3C, 0x00},
    ['9'] = {0x3C, 0x42, 0x42, 0x3E, 0x02, 0x04, 0x38, 0x00},
    ['.'] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x60},
    [','] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x60},
    [':'] = {0x00, 0x00, 0x00, 0x60, 0x60, 0x00, 0x60, 0x60},
    ['-'] = {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00},
    ['!'] = {0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00},
    ['+'] = {0x00, 0x00, 0x18, 0x7E, 0x18, 0x00, 0x00, 0x00},
    ['/'] = {0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x00, 0x00},
    ['('] = {0x0C, 0x10, 0x20, 0x20, 0x20, 0x10, 0x0C, 0x00},
    [')'] = {0x30, 0x08, 0x04, 0x04, 0x04, 0x08, 0x30, 0x00},
    ['='] = {0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00},
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['"'] = {0x24, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['\''] = {0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['?'] = {0x3C, 0x42, 0x02, 0x0C, 0x10, 0x00, 0x10, 0x00},
    [';'] = {0x00, 0x00, 0x00, 0x30, 0x30, 0x10, 0x20, 0x00},
    ['a'] = {0x00, 0x00, 0x3C, 0x02, 0x3E, 0x42, 0x3E, 0x00},
    ['b'] = {0x40, 0x40, 0x7C, 0x42, 0x42, 0x42, 0x7C, 0x00},
    ['c'] = {0x00, 0x00, 0x3C, 0x42, 0x40, 0x42, 0x3C, 0x00},
    ['d'] = {0x02, 0x02, 0x3E, 0x42, 0x42, 0x42, 0x3E, 0x00},
    ['e'] = {0x00, 0x00, 0x3C, 0x42, 0x7E, 0x40, 0x3C, 0x00},
    ['f'] = {0x1C, 0x20, 0x7C, 0x20, 0x20, 0x20, 0x20, 0x00},
    ['g'] = {0x00, 0x00, 0x3E, 0x42, 0x42, 0x3E, 0x02, 0x3C},
    ['h'] = {0x40, 0x40, 0x7C, 0x42, 0x42, 0x42, 0x42, 0x00},
    ['i'] = {0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x7E, 0x00},
    ['j'] = {0x06, 0x00, 0x1E, 0x06, 0x06, 0x06, 0x46, 0x3C},
    ['k'] = {0x40, 0x40, 0x44, 0x48, 0x70, 0x48, 0x44, 0x00},
    ['l'] = {0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00},
    ['m'] = {0x00, 0x00, 0x76, 0x5A, 0x4A, 0x42, 0x42, 0x00},
    ['n'] = {0x00, 0x00, 0x7C, 0x42, 0x42, 0x42, 0x42, 0x00},
    ['o'] = {0x00, 0x00, 0x3C, 0x42, 0x42, 0x42, 0x3C, 0x00},
    ['p'] = {0x00, 0x00, 0x7C, 0x42, 0x42, 0x7C, 0x40, 0x40},
    ['q'] = {0x00, 0x00, 0x3E, 0x42, 0x42, 0x3E, 0x02, 0x02},
    ['r'] = {0x00, 0x00, 0x5C, 0x62, 0x40, 0x40, 0x40, 0x00},
    ['s'] = {0x00, 0x00, 0x3E, 0x40, 0x3C, 0x02, 0x7C, 0x00},
    ['t'] = {0x20, 0x20, 0x7C, 0x20, 0x20, 0x24, 0x18, 0x00},
    ['u'] = {0x00, 0x00, 0x42, 0x42, 0x42, 0x42, 0x3E, 0x00},
    ['v'] = {0x00, 0x00, 0x42, 0x42, 0x42, 0x24, 0x18, 0x00},
    ['w'] = {0x00, 0x00, 0x42, 0x42, 0x4A, 0x5A, 0x24, 0x00},
    ['x'] = {0x00, 0x00, 0x42, 0x24, 0x18, 0x24, 0x42, 0x00},
    ['y'] = {0x00, 0x00, 0x42, 0x42, 0x42, 0x3E, 0x02, 0x3C},
    ['z'] = {0x00, 0x00, 0x7E, 0x04, 0x18, 0x20, 0x7E, 0x00},
    ['#'] = {0x24, 0x24, 0x7E, 0x24, 0x7E, 0x24, 0x24, 0x00},
    ['$'] = {0x08, 0x3E, 0x48, 0x3C, 0x0A, 0x7C, 0x08, 0x00},
    ['%'] = {0x00, 0x42, 0x44, 0x08, 0x10, 0x22, 0x42, 0x00},
    ['&'] = {0x18, 0x24, 0x24, 0x18, 0x28, 0x44, 0x3A, 0x00},
    ['*'] = {0x24, 0x18, 0x7E, 0x18, 0x24, 0x00, 0x00, 0x00},
    ['<'] = {0x04, 0x08, 0x10, 0x20, 0x10, 0x08, 0x04, 0x00},
    ['>'] = {0x20, 0x10, 0x08, 0x04, 0x08, 0x10, 0x20, 0x00},
    ['@'] = {0x3C, 0x42, 0x99, 0xA5, 0x9E, 0x40, 0x3C, 0x00},
    ['['] = {0x3C, 0x20, 0x20, 0x20, 0x20, 0x20, 0x3C, 0x00},
    ['\\'] = {0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x00, 0x00},
    [']'] = {0x3C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x3C, 0x00},
    ['^'] = {0x18, 0x24, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['_'] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x00},
    ['`'] = {0x30, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['{'] = {0x0C, 0x10, 0x10, 0x60, 0x10, 0x10, 0x0C, 0x00},
    ['|'] = {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00},
    ['}'] = {0x60, 0x10, 0x10, 0x0C, 0x10, 0x10, 0x60, 0x00},
    ['~'] = {0x00, 0x00, 0x00, 0x60, 0x92, 0x0C, 0x00, 0x00},
};

// Include anti-aliased font data (defines FONT_AA_W, FONT_AA_H, font_aa_data)
#include "../../lib/font_aa.c"

// Alpha-blend a pixel: blend src_color with alpha (0-15) onto bg
static void gfx_putpixel_alpha(int x, int y, uint32_t color, int alpha)
{
    if (alpha <= 0 || x < 0 || y < 0 || x >= (int)fb.width || y >= (int)fb.height) return;
    if (alpha >= 15) { gfx_putpixel(x, y, color); return; }
    uint32_t *pixel = (uint32_t *)((uint8_t*)fb.framebuffer + y * fb.pitch + x * 4);
    uint32_t bg = *pixel;
    int sa = alpha;
    int da = 15 - alpha;
    int r = (((color >> 16) & 0xFF) * sa + ((bg >> 16) & 0xFF) * da) / 15;
    int g = (((color >> 8) & 0xFF) * sa + ((bg >> 8) & 0xFF) * da) / 15;
    int b = ((color & 0xFF) * sa + (bg & 0xFF) * da) / 15;
    *pixel = (r << 16) | (g << 8) | b;
}

// Draw a semi-transparent rectangle (alpha 0-15)
void gfx_rect_alpha(int x, int y, int w, int h, uint32_t color, int alpha)
{
    if (alpha >= 15) { gfx_rect(x, y, w, h, color); return; }
    if (alpha <= 0) return;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            gfx_putpixel_alpha(xx, yy, color, alpha);
}

// Box blur on a framebuffer region — uses temp buffer per row/column
void gfx_blur_rect(int x, int y, int w, int h, int radius)
{
    if (w <= 0 || h <= 0 || radius <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        uint32_t row[2048];
        int rw = w; if (rw > 2048) rw = 2048;
        for (int xx = 0; xx < rw; xx++) row[xx] = gfx_getpixel(x + xx, yy);
        for (int xx = 0; xx < rw; xx++) {
            int rt = 0, gt = 0, bt = 0, cn = 0;
            for (int d = -radius; d <= radius; d++) {
                int ix = xx + d;
                if (ix < 0 || ix >= rw) continue;
                uint32_t p = row[ix];
                rt += (p >> 16) & 0xFF; gt += (p >> 8) & 0xFF; bt += p & 0xFF;
                cn++;
            }
            if (cn > 0) gfx_putpixel(x + xx, yy, ((rt/cn)<<16) | ((gt/cn)<<8) | (bt/cn));
        }
    }
    for (int xx = x; xx < x + w; xx++) {
        uint32_t col[256];
        int rh = h; if (rh > 256) rh = 256;
        for (int yy = 0; yy < rh; yy++) col[yy] = gfx_getpixel(xx, y + yy);
        for (int yy = 0; yy < rh; yy++) {
            int rt = 0, gt = 0, bt = 0, cn = 0;
            for (int d = -radius; d <= radius; d++) {
                int iy = yy + d;
                if (iy < 0 || iy >= rh) continue;
                uint32_t p = col[iy];
                rt += (p >> 16) & 0xFF; gt += (p >> 8) & 0xFF; bt += p & 0xFF;
                cn++;
            }
            if (cn > 0) gfx_putpixel(xx, y + yy, ((rt/cn)<<16) | ((gt/cn)<<8) | (bt/cn));
        }
    }
}

void gfx_print(int x, int y, uint32_t color, const char *text)
{
    renderer_draw_text(x, y, text, color);
}

void gfx_print_shadow(int x, int y, uint32_t color, const char *text)
{
    renderer_draw_text(x + 1, y + 1, text, 0x000008);
    renderer_draw_text(x, y, text, color);
}

void gfx_print_scaled(int x, int y, uint32_t color, const char *text, int scale)
{
    renderer_draw_text_scaled(x, y, text, color, scale);
}

void gfx_line(int x0, int y0, int x1, int y1, int thick, uint32_t color)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps = dx < 0 ? -dx : dx;
    if (dy < 0 ? -dy : dy > steps) steps = dy < 0 ? -dy : dy;
    if (steps == 0) {
        for (int i = -(thick/2); i <= thick/2; i++)
            for (int j = -(thick/2); j <= thick/2; j++)
                gfx_putpixel(x0+i, y0+j, color);
        return;
    }
    for (int s = 0; s <= steps; s++) {
        int px = x0 + dx * s / steps;
        int py = y0 + dy * s / steps;
        for (int i = -(thick/2); i <= thick/2; i++)
            for (int j = -(thick/2); j <= thick/2; j++)
                gfx_putpixel(px+i, py+j, color);
    }
}

void gfx_blit_alpha(int x, int y, int w, int h, const unsigned char *alpha, uint32_t color)
{
    volatile uint32_t *fb = gfx_get_fb_addr();
    uint32_t pitch = gfx_get_pitch() / 4;
    uint32_t sw = gfx_width(), sh = gfx_height();
    int cr = (color >> 16) & 0xFF;
    int cg = (color >> 8) & 0xFF;
    int cb = color & 0xFF;
    for (int row = 0; row < h; row++) {
        int fy = y + row;
        if (fy < 0 || fy >= (int)sh) continue;
        for (int col = 0; col < w; col++) {
            int fx = x + col;
            if (fx < 0 || fx >= (int)sw) continue;
            unsigned char a = alpha[row * w + col];
            if (a == 0) continue;
            if (a == 255) {
                fb[fy * pitch + fx] = color;
            } else {
                uint32_t dst = fb[fy * pitch + fx];
                int dr = (dst >> 16) & 0xFF;
                int dg = (dst >> 8) & 0xFF;
                int db = dst & 0xFF;
                int r = dr + ((cr - dr) * a >> 8);
                int g = dg + ((cg - dg) * a >> 8);
                int b = db + ((cb - db) * a >> 8);
                fb[fy * pitch + fx] = (r << 16) | (g << 8) | b;
            }
        }
    }
}
