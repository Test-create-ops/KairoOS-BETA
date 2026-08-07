#include "renderer.h"
#include "font_baked.h"
#include "../../kernel/drivers/graphics/gfx.h"

#define RENDERER_TITLE_BAR_H 36

static void fb_putpixel(int x, int y, unsigned int color)
{
    gfx_putpixel(x, y, color);
}

int renderer_window_title_h(const char *title)
{
    (void)title;
    return RENDERER_TITLE_BAR_H;
}

void renderer_draw_pixel(int x, int y, unsigned int color)
{
    fb_putpixel(x, y, color);
}

void renderer_draw_rect(int x, int y, int w, int h, unsigned int color)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            fb_putpixel(xx, yy, color);
}

/*
 * Real-font text rendering: glyphs are baked from a TrueType font (Inter)
 * by tools/fontgen.c and stored packed in font_baked.h. Each glyph is
 * alpha-blended over the framebuffer and advances by its proportional width.
 *
 *   scale == 1   -> regular (16px) size
 *   scale >= 2   -> large (32px) size, integer factor = scale/2
 */
static void draw_text_impl(int x, int y, const char *text, unsigned int color, int scale)
{
    const baked_glyph_t *table = (scale >= 2) ? font_lg : font_reg;
    int factor = (scale >= 2) ? (scale / 2) : 1;
    if (factor < 1) factor = 1;
    int ascent = (scale >= 2) ? FONT_ASCENT_LG : FONT_ASCENT_REG;

    int gw = gfx_width(), gh = gfx_height();
    if (gw <= 0 || gh <= 0) return;
    volatile uint32_t *fb = gfx_get_fb_addr();

    int cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;

    while (*text) {
        unsigned char c = (unsigned char)*text;
        int adv = (scale >= 2) ? 16 : 8;
        if (c >= FONT_CHARSET_START && c <= FONT_CHARSET_END) {
            const baked_glyph_t *g = &table[c - FONT_CHARSET_START];
            int w = g->w, h = g->h;
            int top = g->top;
            const unsigned char *px = font_alpha + g->offset;
            int y0 = y + (ascent - top) * factor;
            adv = g->adv * factor;

            for (int row = 0; row < h; row++) {
                for (int fy = 0; fy < factor; fy++) {
                    int yy = y0 + row * factor + fy;
                    if (yy < 0 || yy >= gh) continue;
                    unsigned int rowbase = (unsigned int)(yy * gw);
                    for (int col = 0; col < w; col++) {
                        int a = px[(unsigned)row * w + col];
                        if (a == 0) continue;
                        for (int fx = 0; fx < factor; fx++) {
                            int xx = x + col * factor + fx;
                            if (xx < 0 || xx >= gw) continue;
                            unsigned int dst = rowbase + (unsigned int)xx;
                            unsigned int bg = fb[dst];
                            int nr = (cr * a + (int)((bg >> 16) & 0xFF) * (255 - a)) / 255;
                            int ng = (cg * a + (int)((bg >> 8) & 0xFF) * (255 - a)) / 255;
                            int nb = (cb * a + (int)(bg & 0xFF) * (255 - a)) / 255;
                            fb[dst] = ((unsigned)nr << 16) | ((unsigned)ng << 8) | (unsigned)nb;
                        }
                    }
                }
            }
        }
        x += adv;
        text++;
    }
}

void renderer_draw_text(int x, int y, const char *text, unsigned int color)
{
    draw_text_impl(x, y, text, color, 1);
}

void renderer_draw_text_scaled(int x, int y, const char *text, unsigned int color, int scale)
{
    draw_text_impl(x, y, text, color, scale);
}

#define CORNER_SS 16

static int subpixel_inside(int w, int h, int R, int dx, int dy, int sx, int sy)
{
    int px = dx * CORNER_SS + sx + 1;
    int py = dy * CORNER_SS + sy + 1;
    int ex = w * CORNER_SS - 1 - px;
    int by = h * CORNER_SS - 1 - py;

    if (px < R && py < R) {
        int cx = px - R, cy = py - R;
        if (cx * cx + cy * cy > R * R) return 0;
    }
    if (ex < R && py < R) {
        int cx = ex - R, cy = py - R;
        if (cx * cx + cy * cy > R * R) return 0;
    }
    if (px < R && by < R) {
        int cx = px - R, cy = by - R;
        if (cx * cx + cy * cy > R * R) return 0;
    }
    if (ex < R && by < R) {
        int cx = ex - R, cy = by - R;
        if (cx * cx + cy * cy > R * R) return 0;
    }
    return 1;
}

static unsigned int blend(unsigned int a, unsigned int o, int num, int den)
{
    int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
    int orr = (o >> 16) & 0xff, og = (o >> 8) & 0xff, ob = o & 0xff;
    int r = (ar * num + orr * (den - num)) / den;
    int g = (ag * num + og * (den - num)) / den;
    int b = (ab * num + ob * (den - num)) / den;
    return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

/* Integer point-in-rounded-rect test (no subpixel AA). */
static int rect_inside(int dx, int dy, int w, int h, int r)
{
    int ex = w - 1 - dx, by = h - 1 - dy;
    if (dx < r && dy < r) { int cx = r - 1 - dx, cy = r - 1 - dy; if (cx*cx + cy*cy > r*r) return 0; }
    if (ex < r && dy < r) { int cx = r - 1 - ex, cy = r - 1 - dy; if (cx*cx + cy*cy > r*r) return 0; }
    if (dx < r && by < r) { int cx = r - 1 - dx, cy = r - 1 - by; if (cx*cx + cy*cy > r*r) return 0; }
    if (ex < r && by < r) { int cx = r - 1 - ex, cy = r - 1 - by; if (cx*cx + cy*cy > r*r) return 0; }
    return 1;
}

/* Soft drop shadow around a rounded rectangle (before the window body). */
static void renderer_draw_window_shadow(int x, int y, int w, int h)
{
    const int LAYERS = 6;
    const int r = 9;
    int gw = gfx_width(), gh = gfx_height();
    if (gw <= 0 || gh <= 0) return;
    volatile uint32_t *fb = gfx_get_fb_addr();

    for (int l = 1; l <= LAYERS; l++) {
        int off = l;
        int a = 20 + l * 18;
        int inv = 255 - a;
        int ex = x - off, ey = y - off, ew = w + 2 * off, eh = h + 2 * off, er = r + off;

        for (int yy = ey; yy < ey + eh; yy++) {
            if (yy < 0 || yy >= gh) continue;
            unsigned int rowbase = (unsigned int)(yy * gw);
            for (int xx = ex; xx < ex + ew; xx++) {
                if (xx < 0 || xx >= gw) continue;
                if (!rect_inside(xx - ex, yy - ey, ew, eh, er)) continue;
                if (rect_inside(xx - x, yy - y, w, h, r)) continue;
                unsigned int dst = rowbase + (unsigned int)xx;
                unsigned int bg = fb[dst];
                int nr = (int)((bg >> 16) & 0xFF) * inv / 255;
                int ng = (int)((bg >> 8) & 0xFF) * inv / 255;
                int nb = (int)(bg & 0xFF) * inv / 255;
                fb[dst] = ((unsigned)nr << 16) | ((unsigned)ng << 8) | (unsigned)nb;
            }
        }
    }
}

static void renderer_fill_circle(int cx, int cy, int r, unsigned int color)
{
    for (int yy = cy - r; yy <= cy + r; yy++) {
        if (yy < 0 || yy >= gfx_height()) continue;
        for (int xx = cx - r; xx <= cx + r; xx++) {
            if (xx < 0 || xx >= gfx_width()) continue;
            int dx = xx - cx, dy = yy - cy;
            if (dx * dx + dy * dy <= r * r) fb_putpixel(xx, yy, color);
        }
    }
}

void renderer_draw_window(int x, int y, int w, int h, const char *title,
                          unsigned int title_color, unsigned int bg_color,
                          unsigned int text_color)
{
    renderer_draw_window_shadow(x, y, w, h);

    int bar_h = renderer_window_title_h(title);
    int r = 9;
    int R = r * CORNER_SS;
    int N = CORNER_SS * CORNER_SS;

    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            int win_color = (yy < y + bar_h) ? title_color : bg_color;
            if (yy < y + bar_h) {
                /* subtle vertical gradient in the title bar (lighter on top) */
                int boost = (bar_h - (yy - y)) * 18 / bar_h;
                int tr = (title_color >> 16) & 0xff, tg = (title_color >> 8) & 0xff, tb = title_color & 0xff;
                tr = tr + ((255 - tr) * boost) / 100;
                tg = tg + ((255 - tg) * boost) / 100;
                tb = tb + ((255 - tb) * boost) / 100;
                win_color = ((unsigned)tr << 16) | ((unsigned)tg << 8) | (unsigned)tb;
            }
            int dx = xx - x;
            int ex = (x + w - 1) - xx;
            int dy = yy - y;
            int by = (y + h - 1) - yy;

            if (dy >= r && by >= r && dx >= r && ex >= r) {
                fb_putpixel(xx, yy, win_color);
                continue;
            }

            int cov = 0;
            for (int sy = 0; sy < CORNER_SS; sy++)
                for (int sx = 0; sx < CORNER_SS; sx++)
                    if (subpixel_inside(w, h, R, dx, dy, sx, sy))
                        cov++;

            if (cov == N) fb_putpixel(xx, yy, win_color);
            else if (cov > 0) {
                if (yy < y + bar_h) {
                    unsigned int fc = ((((win_color >> 16) & 0xff) * 55 / 100) << 16) |
                                      ((((win_color >> 8) & 0xff) * 55 / 100) << 8) |
                                      (((win_color & 0xff) * 55 / 100));
                    fb_putpixel(xx, yy, blend(win_color, fc, cov, N));
                } else {
                    unsigned int under = gfx_getpixel(xx, yy);
                    fb_putpixel(xx, yy, blend(win_color, under, cov, N));
                }
            }
        }
    }

    if (title) {
        /* macOS traffic-light buttons */
        int ly = y + (bar_h - 12) / 2;
        renderer_fill_circle(x + 16, ly + 6, 6, 0xFF5F57);
        renderer_fill_circle(x + 36, ly + 6, 6, 0xFEBC2E);
        renderer_fill_circle(x + 56, ly + 6, 6, 0x28C840);
        int tcy = y + (bar_h - FONT_ASCENT_REG) / 2 + 2;
        renderer_draw_text(x + 68, tcy, title, text_color);
    }
}
